use super::{CorexError, ProcessInfo, ThreadState};

const NOTE_ALIGNMENT: usize = 4;
const NT_PRSTATUS: u32 = 1;
const NT_FPREGSET: u32 = 2;
const NT_PRPSINFO: u32 = 3;
const NT_AUXV: u32 = 6;
const NT_SIGINFO: u32 = 0x5349_4749;
const NT_FILE: u32 = 0x4649_4c45;
const NT_ARM_PAC_MASK: u32 = 0x406;
const PAGE_SIZE: u64 = 4096;
const PRSTATUS_REG_OFFSET: usize = 112;

pub(super) fn build(process: &ProcessInfo, threads: &[ThreadState]) -> Result<Vec<u8>, CorexError> {
    let mut notes = Vec::with_capacity(256 * 1024);
    for thread in threads {
        append(
            &mut notes,
            b"CORE",
            NT_PRSTATUS,
            &prstatus(process, thread)?,
        )?;
        append(&mut notes, b"CORE", NT_FPREGSET, &thread.fp_regs)?;
        if let Some(mask) = thread.pac_mask {
            let mut descriptor = Vec::with_capacity(16);
            descriptor.extend_from_slice(&mask[0].to_ne_bytes());
            descriptor.extend_from_slice(&mask[1].to_ne_bytes());
            append(&mut notes, b"LINUX", NT_ARM_PAC_MASK, &descriptor)?;
        }
    }
    append(&mut notes, b"CORE", NT_PRPSINFO, &prpsinfo(process))?;
    append(&mut notes, b"CORE", NT_SIGINFO, &[0_u8; 128])?;
    if !process.auxv.is_empty() {
        append(&mut notes, b"CORE", NT_AUXV, &process.auxv)?;
    }
    if let Some(file_note) = file_note(process) {
        append(&mut notes, b"CORE", NT_FILE, &file_note)?;
    }
    Ok(notes)
}

fn append(
    notes: &mut Vec<u8>,
    owner: &[u8],
    note_type: u32,
    descriptor: &[u8],
) -> Result<(), CorexError> {
    let name_size = owner
        .len()
        .checked_add(1)
        .ok_or_else(|| CorexError::InvalidData("ELF note owner is too large".into()))?;
    let descriptor_size = u32::try_from(descriptor.len())
        .map_err(|_| CorexError::InvalidData("ELF note descriptor is too large".into()))?;
    notes.extend_from_slice(&(name_size as u32).to_ne_bytes());
    notes.extend_from_slice(&descriptor_size.to_ne_bytes());
    notes.extend_from_slice(&note_type.to_ne_bytes());
    notes.extend_from_slice(owner);
    notes.push(0);
    pad(notes, NOTE_ALIGNMENT);
    notes.extend_from_slice(descriptor);
    pad(notes, NOTE_ALIGNMENT);
    Ok(())
}

fn prstatus(process: &ProcessInfo, thread: &ThreadState) -> Result<Vec<u8>, CorexError> {
    if thread.gp_regs.len() != gp_regset_size() {
        return Err(CorexError::InvalidData(format!(
            "thread {} GP register size is {}, expected {}",
            thread.tid,
            thread.gp_regs.len(),
            gp_regset_size()
        )));
    }
    let mut status = vec![0_u8; prstatus_size()];
    write_i32(&mut status, 32, thread.tid);
    write_i32(&mut status, 36, process.pid);
    write_i32(&mut status, 40, process.pid);
    write_i32(&mut status, 44, process.pid);
    status[PRSTATUS_REG_OFFSET..PRSTATUS_REG_OFFSET + thread.gp_regs.len()]
        .copy_from_slice(&thread.gp_regs);
    Ok(status)
}

fn prpsinfo(process: &ProcessInfo) -> Vec<u8> {
    let mut info = vec![0_u8; 136];
    info[1] = b'R';
    write_u32(&mut info, 16, process.uid);
    write_u32(&mut info, 20, process.gid);
    write_i32(&mut info, 24, process.pid);
    write_i32(&mut info, 28, process.ppid);
    write_i32(&mut info, 32, process.pgrp);
    write_i32(&mut info, 36, process.sid);
    copy_truncated(&mut info[40..56], &process.comm);

    let mut arguments: Vec<_> = process
        .cmdline
        .iter()
        .map(|byte| if *byte == 0 { b' ' } else { *byte })
        .take(79)
        .collect();
    while arguments.last() == Some(&b' ') {
        arguments.pop();
    }
    copy_truncated(&mut info[56..136], &arguments);
    info
}

fn file_note(process: &ProcessInfo) -> Option<Vec<u8>> {
    let mappings: Vec<_> = process
        .mappings
        .iter()
        .filter(|mapping| mapping.path.starts_with('/'))
        .collect();
    if mappings.is_empty() {
        return None;
    }
    let names_size: usize = mappings.iter().map(|mapping| mapping.path.len() + 1).sum();
    let mut descriptor = Vec::with_capacity(16 + mappings.len() * 24 + names_size);
    descriptor.extend_from_slice(&(mappings.len() as u64).to_ne_bytes());
    descriptor.extend_from_slice(&PAGE_SIZE.to_ne_bytes());
    for mapping in &mappings {
        descriptor.extend_from_slice(&mapping.start.to_ne_bytes());
        descriptor.extend_from_slice(&mapping.end.to_ne_bytes());
        descriptor.extend_from_slice(&(mapping.offset / PAGE_SIZE).to_ne_bytes());
    }
    for mapping in mappings {
        descriptor.extend_from_slice(mapping.path.as_bytes());
        descriptor.push(0);
    }
    Some(descriptor)
}

fn copy_truncated(output: &mut [u8], value: &[u8]) {
    let length = value.len().min(output.len().saturating_sub(1));
    output[..length].copy_from_slice(&value[..length]);
}

fn write_i32(output: &mut [u8], offset: usize, value: i32) {
    output[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn write_u32(output: &mut [u8], offset: usize, value: u32) {
    output[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn pad(output: &mut Vec<u8>, alignment: usize) {
    let aligned = output.len().div_ceil(alignment) * alignment;
    output.resize(aligned, 0);
}

#[cfg(target_arch = "aarch64")]
const fn gp_regset_size() -> usize {
    272
}

#[cfg(target_arch = "aarch64")]
const fn prstatus_size() -> usize {
    392
}

#[cfg(target_arch = "x86_64")]
const fn gp_regset_size() -> usize {
    216
}

#[cfg(target_arch = "x86_64")]
const fn prstatus_size() -> usize {
    336
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::corex::Mapping;

    fn process() -> ProcessInfo {
        ProcessInfo {
            pid: 42,
            ppid: 1,
            pgrp: 42,
            sid: 42,
            uid: 1000,
            gid: 1000,
            comm: b"worker".to_vec(),
            exe: "/tmp/worker".into(),
            cmdline: b"worker\0run\0".to_vec(),
            mappings: vec![Mapping {
                start: 0x1000,
                end: 0x2000,
                flags: 5,
                offset: 0,
                is_shared: false,
                is_file_backed: true,
                should_dump: true,
                path: "/tmp/worker".into(),
            }],
            auxv: vec![0; 16],
            coredump_filter: 0x33,
            tids: vec![42],
        }
    }

    #[test]
    fn prpsinfo_matches_linux_abi_offsets() {
        let info = prpsinfo(&process());
        assert_eq!(info.len(), 136);
        assert_eq!(&info[24..28], &42_i32.to_ne_bytes());
        assert_eq!(&info[40..46], b"worker");
        assert_eq!(&info[56..66], b"worker run");
    }

    #[test]
    fn file_note_uses_page_offsets() {
        let descriptor = file_note(&process()).unwrap();
        assert_eq!(&descriptor[0..8], &1_u64.to_ne_bytes());
        assert_eq!(&descriptor[8..16], &PAGE_SIZE.to_ne_bytes());
        assert!(descriptor.ends_with(b"/tmp/worker\0"));
    }
}
