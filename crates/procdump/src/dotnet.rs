use std::collections::HashSet;
use std::fs;
use std::io::{self, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::Duration;

const IPC_HEADER_SIZE: usize = 20;
const IPC_RESPONSE_SIZE: u16 = 24;
const DUMP_COMMAND_SET: u8 = 0x01;
const DUMP_COMMAND_ID: u8 = 0x01;
const FULL_DUMP_TYPE: u32 = 4;
const DUMP_LOGGING_OFF: u32 = 0;

pub fn find_diagnostics_socket(pid: i32) -> Result<Option<PathBuf>, DotNetError> {
    let socket_table = fs::read_to_string("/proc/net/unix").map_err(DotNetError::SocketTable)?;
    let temporary = std::env::var_os("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    let prefix = temporary.join(format!("dotnet-diagnostic-{pid}-"));
    let inodes = process_socket_inodes(pid)?;
    Ok(find_socket_in_table(&socket_table, &prefix, &inodes))
}

fn process_socket_inodes(pid: i32) -> Result<HashSet<u64>, DotNetError> {
    let directory = format!("/proc/{pid}/fd");
    let entries = fs::read_dir(&directory).map_err(DotNetError::Descriptors)?;
    let mut inodes = HashSet::new();
    for entry in entries {
        let Ok(entry) = entry else { continue };
        let Ok(target) = fs::read_link(entry.path()) else {
            continue;
        };
        let target = target.to_string_lossy();
        if let Some(inode) = target
            .strip_prefix("socket:[")
            .and_then(|value| value.strip_suffix(']'))
            .and_then(|value| value.parse().ok())
        {
            inodes.insert(inode);
        }
    }
    Ok(inodes)
}

pub(crate) fn generate_dump(socket: &Path, output: &Path) -> Result<(), DotNetError> {
    let packet = build_dump_packet(output)?;
    let mut stream = UnixStream::connect(socket).map_err(|source| DotNetError::Connect {
        socket: socket.to_path_buf(),
        source,
    })?;
    let timeout = Some(Duration::from_secs(60));
    stream
        .set_read_timeout(timeout)
        .map_err(DotNetError::Configure)?;
    stream
        .set_write_timeout(timeout)
        .map_err(DotNetError::Configure)?;
    stream.write_all(&packet).map_err(DotNetError::Send)?;

    let mut header = [0_u8; IPC_HEADER_SIZE];
    stream
        .read_exact(&mut header)
        .map_err(DotNetError::Receive)?;
    let response_size = u16::from_le_bytes([header[14], header[15]]);
    if response_size != IPC_RESPONSE_SIZE {
        return Err(DotNetError::InvalidResponse(format!(
            "response size {response_size}, expected {IPC_RESPONSE_SIZE}"
        )));
    }
    let mut result = [0_u8; 4];
    stream
        .read_exact(&mut result)
        .map_err(DotNetError::Receive)?;
    let result = i32::from_le_bytes(result);
    if result != 0 {
        return Err(DotNetError::Runtime(result));
    }
    Ok(())
}

fn find_socket_in_table(
    table: &str,
    prefix: &Path,
    target_inodes: &HashSet<u64>,
) -> Option<PathBuf> {
    let prefix = prefix.to_string_lossy();
    table
        .lines()
        .filter_map(|line| {
            let fields: Vec<_> = line.split_whitespace().collect();
            let inode = fields.get(6)?.parse::<u64>().ok()?;
            let path = *fields.get(7)?;
            (target_inodes.contains(&inode) && path.starts_with(prefix.as_ref()))
                .then(|| PathBuf::from(path))
        })
        .next()
}

fn build_dump_packet(output: &Path) -> Result<Vec<u8>, DotNetError> {
    let output = output
        .to_str()
        .ok_or_else(|| DotNetError::InvalidPath(output.to_path_buf()))?;
    let mut output_utf16: Vec<u16> = output.encode_utf16().collect();
    output_utf16.push(0);
    let character_count =
        u32::try_from(output_utf16.len()).map_err(|_| DotNetError::InvalidPath(output.into()))?;
    let payload_size = 4 + output_utf16.len() * 2 + 4 + 4;
    let packet_size = IPC_HEADER_SIZE
        .checked_add(payload_size)
        .and_then(|size| u16::try_from(size).ok())
        .ok_or_else(|| DotNetError::InvalidPath(output.into()))?;

    let mut packet = Vec::with_capacity(packet_size as usize);
    packet.extend_from_slice(b"DOTNET_IPC_V1\0");
    packet.extend_from_slice(&packet_size.to_le_bytes());
    packet.push(DUMP_COMMAND_SET);
    packet.push(DUMP_COMMAND_ID);
    packet.extend_from_slice(&0_u16.to_le_bytes());
    packet.extend_from_slice(&character_count.to_le_bytes());
    for character in output_utf16 {
        packet.extend_from_slice(&character.to_le_bytes());
    }
    packet.extend_from_slice(&FULL_DUMP_TYPE.to_le_bytes());
    packet.extend_from_slice(&DUMP_LOGGING_OFF.to_le_bytes());
    Ok(packet)
}

#[derive(Debug)]
pub enum DotNetError {
    SocketTable(io::Error),
    Descriptors(io::Error),
    Connect { socket: PathBuf, source: io::Error },
    Configure(io::Error),
    Send(io::Error),
    Receive(io::Error),
    InvalidPath(PathBuf),
    InvalidResponse(String),
    Runtime(i32),
}

impl std::fmt::Display for DotNetError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::SocketTable(error) => write!(formatter, "failed to read /proc/net/unix: {error}"),
            Self::Descriptors(error) => {
                write!(
                    formatter,
                    "failed to inspect target socket descriptors: {error}"
                )
            }
            Self::Connect { socket, source } => write!(
                formatter,
                "failed to connect to .NET diagnostics socket {}: {source}",
                socket.display()
            ),
            Self::Configure(error) => {
                write!(formatter, "failed to configure diagnostics socket: {error}")
            }
            Self::Send(error) => write!(formatter, "failed to send .NET dump request: {error}"),
            Self::Receive(error) => {
                write!(formatter, "failed to receive .NET dump response: {error}")
            }
            Self::InvalidPath(path) => {
                write!(formatter, "invalid .NET dump path: {}", path.display())
            }
            Self::InvalidResponse(detail) => {
                write!(formatter, "invalid .NET diagnostics response: {detail}")
            }
            Self::Runtime(result) => write!(
                formatter,
                ".NET runtime dump request failed with HRESULT 0x{result:08x}"
            ),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn socket_matching_requires_pid_delimiter() {
        let table = "Num RefCount Protocol Flags Type St Inode Path\n\
                     0: 1 2 3 4 5 6 /tmp/dotnet-diagnostic-1168-1-socket\n\
                     1: 1 2 3 4 5 6 /tmp/dotnet-diagnostic-1-9-socket\n";
        assert_eq!(
            find_socket_in_table(
                table,
                Path::new("/tmp/dotnet-diagnostic-1-"),
                &HashSet::from([6]),
            ),
            Some(PathBuf::from("/tmp/dotnet-diagnostic-1-9-socket"))
        );
    }

    #[test]
    fn dump_packet_has_expected_header_and_utf16_payload() {
        let packet = build_dump_packet(Path::new("/tmp/core")).unwrap();

        assert_eq!(&packet[..14], b"DOTNET_IPC_V1\0");
        assert_eq!(
            u16::from_le_bytes([packet[14], packet[15]]) as usize,
            packet.len()
        );
        assert_eq!(packet[16], DUMP_COMMAND_SET);
        assert_eq!(packet[17], DUMP_COMMAND_ID);
        assert_eq!(u32::from_le_bytes(packet[20..24].try_into().unwrap()), 10);
    }
}
