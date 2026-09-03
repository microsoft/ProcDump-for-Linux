use super::{CorexError, Mapping, ProcessInfo};
use std::fs::{self, File};
use std::io::Write;
use std::os::unix::fs::FileExt;
use std::path::Path;

const ELF_HEADER_SIZE: u64 = 64;
const PROGRAM_HEADER_SIZE: u64 = 56;
const MEMORY_CHUNK_SIZE: usize = 1024 * 1024;
const PT_LOAD: u32 = 1;
const PT_NOTE: u32 = 4;

pub(super) fn write(
    path: &Path,
    output: File,
    process: &ProcessInfo,
    notes: &[u8],
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    let result = write_inner(path, output, process, notes, cancellation);
    if result.is_err() {
        let _ = fs::remove_file(path);
    }
    result
}

fn write_inner(
    path: &Path,
    mut output: File,
    process: &ProcessInfo,
    notes: &[u8],
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    let dumped: Vec<_> = process
        .mappings
        .iter()
        .filter(|mapping| mapping.should_dump)
        .collect();
    let program_header_count = dumped
        .len()
        .checked_add(1)
        .ok_or_else(|| CorexError::InvalidData("too many ELF program headers".into()))?;
    let program_header_count = u16::try_from(program_header_count)
        .map_err(|_| CorexError::InvalidData("too many ELF program headers".into()))?;
    let note_offset = ELF_HEADER_SIZE
        .checked_add(PROGRAM_HEADER_SIZE * u64::from(program_header_count))
        .ok_or_else(|| CorexError::InvalidData("ELF note offset overflow".into()))?;
    let first_load_offset = align_page(
        note_offset
            .checked_add(notes.len() as u64)
            .ok_or_else(|| CorexError::InvalidData("ELF note size overflow".into()))?,
        process.page_size,
    )?;
    let mut next_offset = first_load_offset;
    let mut load_offsets = Vec::with_capacity(dumped.len());
    for mapping in &dumped {
        let size = mapping_size(mapping)?;
        load_offsets.push(next_offset);
        next_offset = next_offset
            .checked_add(size)
            .ok_or_else(|| CorexError::InvalidData("ELF load offset overflow".into()))?;
    }

    let display_path = path.display().to_string();
    output
        .write_all(&elf_header(program_header_count))
        .map_err(|source| CorexError::io("write", &display_path, source))?;
    output
        .write_all(&program_header(
            PT_NOTE,
            0,
            note_offset,
            0,
            notes.len() as u64,
            0,
            4,
        ))
        .map_err(|source| CorexError::io("write", &display_path, source))?;
    for (mapping, offset) in dumped.iter().zip(&load_offsets) {
        let size = mapping_size(mapping)?;
        output
            .write_all(&program_header(
                PT_LOAD,
                mapping.flags,
                *offset,
                mapping.start,
                size,
                size,
                process.page_size,
            ))
            .map_err(|source| CorexError::io("write", &display_path, source))?;
    }
    output
        .write_all(notes)
        .map_err(|source| CorexError::io("write", &display_path, source))?;
    write_zeroes(
        &mut output,
        usize::try_from(first_load_offset - note_offset - notes.len() as u64)
            .map_err(|_| CorexError::InvalidData("ELF padding is too large".into()))?,
        &display_path,
    )?;

    let memory_path = format!("/proc/{}/mem", process.pid);
    let memory =
        File::open(&memory_path).map_err(|source| CorexError::io("open", &memory_path, source))?;
    for mapping in dumped {
        check_cancelled(cancellation)?;
        write_mapping(&mut output, &memory, mapping, &display_path, cancellation)?;
    }
    output
        .flush()
        .map_err(|source| CorexError::io("flush", &display_path, source))
}

fn elf_header(program_header_count: u16) -> [u8; ELF_HEADER_SIZE as usize] {
    let mut header = [0_u8; ELF_HEADER_SIZE as usize];
    header[0..4].copy_from_slice(b"\x7fELF");
    header[4] = 2;
    header[5] = 1;
    header[6] = 1;
    put_u16(&mut header, 16, 4);
    put_u16(&mut header, 18, elf_machine());
    put_u32(&mut header, 20, 1);
    put_u64(&mut header, 32, ELF_HEADER_SIZE);
    put_u16(&mut header, 52, ELF_HEADER_SIZE as u16);
    put_u16(&mut header, 54, PROGRAM_HEADER_SIZE as u16);
    put_u16(&mut header, 56, program_header_count);
    header
}

fn program_header(
    segment_type: u32,
    flags: u32,
    offset: u64,
    virtual_address: u64,
    file_size: u64,
    memory_size: u64,
    alignment: u64,
) -> [u8; PROGRAM_HEADER_SIZE as usize] {
    let mut header = [0_u8; PROGRAM_HEADER_SIZE as usize];
    put_u32(&mut header, 0, segment_type);
    put_u32(&mut header, 4, flags);
    put_u64(&mut header, 8, offset);
    put_u64(&mut header, 16, virtual_address);
    put_u64(&mut header, 32, file_size);
    put_u64(&mut header, 40, memory_size);
    put_u64(&mut header, 48, alignment);
    header
}

fn write_mapping(
    output: &mut File,
    memory: &File,
    mapping: &Mapping,
    output_path: &str,
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    let mut buffer = vec![0_u8; MEMORY_CHUNK_SIZE];
    let mut address = mapping.start;
    let mut remaining = mapping_size(mapping)?;
    while remaining > 0 {
        check_cancelled(cancellation)?;
        let requested = usize::try_from(remaining.min(MEMORY_CHUNK_SIZE as u64)).unwrap();
        let read = match memory.read_at(&mut buffer[..requested], address) {
            Ok(0) => {
                buffer[..requested].fill(0);
                requested
            }
            Ok(read) => read,
            Err(error) if matches!(error.raw_os_error(), Some(libc::EIO | libc::EFAULT)) => {
                buffer[..requested].fill(0);
                requested
            }
            Err(error) => {
                return Err(CorexError::io(
                    "read process memory for",
                    format!("{:#x}-{:#x}", mapping.start, mapping.end),
                    error,
                ));
            }
        };
        output
            .write_all(&buffer[..read])
            .map_err(|source| CorexError::io("write", output_path, source))?;
        address += read as u64;
        remaining -= read as u64;
    }
    Ok(())
}

fn check_cancelled(
    cancellation: Option<&crate::engine::CancellationToken>,
) -> Result<(), CorexError> {
    if cancellation.is_some_and(crate::engine::CancellationToken::is_cancelled) {
        Err(CorexError::Cancelled)
    } else {
        Ok(())
    }
}

fn write_zeroes(output: &mut File, mut size: usize, path: &str) -> Result<(), CorexError> {
    let zeroes = [0_u8; 4096];
    while size > 0 {
        let count = size.min(zeroes.len());
        output
            .write_all(&zeroes[..count])
            .map_err(|source| CorexError::io("write", path, source))?;
        size -= count;
    }
    Ok(())
}

fn mapping_size(mapping: &Mapping) -> Result<u64, CorexError> {
    mapping.end.checked_sub(mapping.start).ok_or_else(|| {
        CorexError::InvalidData(format!(
            "invalid process mapping {:#x}-{:#x}",
            mapping.start, mapping.end
        ))
    })
}

fn align_page(value: u64, page_size: u64) -> Result<u64, CorexError> {
    if page_size == 0 {
        return Err(CorexError::InvalidData("page size is zero".into()));
    }
    value
        .checked_add(page_size - 1)
        .map(|value| value / page_size * page_size)
        .ok_or_else(|| CorexError::InvalidData("ELF alignment overflow".into()))
}

fn put_u16(output: &mut [u8], offset: usize, value: u16) {
    output[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(output: &mut [u8], offset: usize, value: u32) {
    output[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(output: &mut [u8], offset: usize, value: u64) {
    output[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

#[cfg(target_arch = "aarch64")]
const fn elf_machine() -> u16 {
    183
}

#[cfg(target_arch = "x86_64")]
const fn elf_machine() -> u16 {
    62
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn core_header_has_expected_elf_identity() {
        let header = elf_header(3);
        assert_eq!(&header[..4], b"\x7fELF");
        assert_eq!(u16::from_le_bytes(header[16..18].try_into().unwrap()), 4);
        assert_eq!(u16::from_le_bytes(header[56..58].try_into().unwrap()), 3);
    }

    #[test]
    fn page_alignment_rounds_up() {
        assert_eq!(align_page(16_384, 16_384).unwrap(), 16_384);
        assert_eq!(align_page(16_385, 16_384).unwrap(), 32_768);
    }
}
