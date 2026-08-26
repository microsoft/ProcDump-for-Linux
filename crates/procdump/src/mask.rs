use crate::WriteDumpError;

#[cfg(target_os = "linux")]
use std::fs;
#[cfg(target_os = "linux")]
use std::path::PathBuf;

pub(crate) struct CoreDumpMaskGuard {
    #[cfg(target_os = "linux")]
    path: Option<PathBuf>,
    #[cfg(target_os = "linux")]
    previous: Option<u32>,
}

impl CoreDumpMaskGuard {
    #[cfg(target_os = "linux")]
    pub(crate) fn apply(pid: i32, mask: Option<u32>) -> Result<Self, WriteDumpError> {
        let Some(mask) = mask else {
            return Ok(Self {
                path: None,
                previous: None,
            });
        };
        let path = PathBuf::from(format!("/proc/{pid}/coredump_filter"));
        let previous_text = fs::read_to_string(&path).map_err(|source| WriteDumpError::Io {
            operation: "read",
            path: path.clone(),
            source,
        })?;
        let previous = u32::from_str_radix(previous_text.trim(), 16).map_err(|_| {
            WriteDumpError::Process(format!(
                "Failed to parse core dump mask from {}: {}",
                path.display(),
                previous_text.trim()
            ))
        })?;
        fs::write(&path, mask_value(mask)).map_err(|source| WriteDumpError::Io {
            operation: "write",
            path: path.clone(),
            source,
        })?;
        Ok(Self {
            path: Some(path),
            previous: Some(previous),
        })
    }

    #[cfg(target_os = "macos")]
    pub(crate) fn apply(_pid: i32, mask: Option<u32>) -> Result<Self, WriteDumpError> {
        if mask.is_some() {
            Err(WriteDumpError::UnsupportedCoreDumpMask)
        } else {
            Ok(Self {})
        }
    }

    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    pub(crate) fn apply(_pid: i32, _mask: Option<u32>) -> Result<Self, WriteDumpError> {
        Err(WriteDumpError::UnsupportedPlatform)
    }
}

impl Drop for CoreDumpMaskGuard {
    fn drop(&mut self) {
        #[cfg(target_os = "linux")]
        if let (Some(path), Some(previous)) = (&self.path, self.previous) {
            let _ = fs::write(path, mask_value(previous));
        }
    }
}

#[cfg(target_os = "linux")]
fn mask_value(mask: u32) -> String {
    mask.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_os = "linux")]
    #[test]
    fn formats_mask_for_kernel_base_detection() {
        assert_eq!(mask_value(0x7f), "127");
    }
}
