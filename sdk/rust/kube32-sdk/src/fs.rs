//! Pod storage on the node's microSD card (`k32_fs_*`, feature `fs`).
//!
//! The pod manifest must request `hardware.kube32.io/sd: "1"` and name one
//! volume claim; every path here is relative to that volume
//! (`/sd/kube32/volumes/<claim>/` on the board, `$KUBE32_SD_ROOT/kube32/volumes/<claim>/`
//! on the host simulator). Absolute paths, `..`, `.`, empty components and
//! characters outside `[A-Za-z0-9._-]` are rejected with [`Error::Invalid`];
//! paths over 96 bytes with [`Error::NameTooLong`]. Without the grant every
//! call returns [`Error::Permission`]; on a node without a card,
//! [`Error::NoDevice`]. See `docs/sd-card.md`.
//!
//! A pod holds at most 4 open files and the node 4 in total, so keep files
//! open only as long as needed; [`File`] closes itself when dropped.

use crate::abi;
use crate::Error;

/// Bytes moved by one `k32_fs_read` / `k32_fs_write` / `k32_fs_list` call.
pub const MAX_IO: usize = 4096;
/// Longest path, relative to the volume root.
pub const MAX_PATH: usize = 96;
/// Open files per pod.
pub const MAX_FDS: usize = 4;

pub const O_RDONLY: u32 = 0x0000;
pub const O_WRONLY: u32 = 0x0001;
pub const O_RDWR: u32 = 0x0002;
pub const O_CREAT: u32 = 0x0040;
pub const O_EXCL: u32 = 0x0080;
pub const O_TRUNC: u32 = 0x0200;
pub const O_APPEND: u32 = 0x0400;

/// An open file in the pod's volume. Closed on drop.
pub struct File {
    fd: i32,
}

impl File {
    /// `k32_fs_open` with explicit `O_*` flags.
    pub fn open_with(path: &str, flags: u32) -> Result<File, Error> {
        let rc = unsafe { abi::k32_fs_open(path.as_ptr(), path.len() as u32, flags) };
        Error::from_rc(rc).map(|fd| File { fd: fd as i32 })
    }

    /// Opens an existing file for reading.
    pub fn open(path: &str) -> Result<File, Error> {
        Self::open_with(path, O_RDONLY)
    }

    /// Creates the file, or truncates it, for writing.
    pub fn create(path: &str) -> Result<File, Error> {
        Self::open_with(path, O_WRONLY | O_CREAT | O_TRUNC)
    }

    /// Opens the file for appending, creating it when missing.
    pub fn append(path: &str) -> Result<File, Error> {
        Self::open_with(path, O_WRONLY | O_CREAT | O_APPEND)
    }

    /// Reads up to `buf.len()` bytes (at most [`MAX_IO`]); `Ok(0)` at end of file.
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize, Error> {
        let len = buf.len().min(MAX_IO) as u32;
        let rc = unsafe { abi::k32_fs_read(self.fd, buf.as_mut_ptr(), len) };
        Error::from_rc(rc).map(|n| n as usize)
    }

    /// One `k32_fs_write` call: at most [`MAX_IO`] bytes; returns how many were written.
    pub fn write(&mut self, data: &[u8]) -> Result<usize, Error> {
        let len = data.len().min(MAX_IO) as u32;
        let rc = unsafe { abi::k32_fs_write(self.fd, data.as_ptr(), len) };
        Error::from_rc(rc).map(|n| n as usize)
    }

    /// Writes all of `data`, looping over [`MAX_IO`] chunks. A short write
    /// (card full) is reported as [`Error::NoSpace`].
    pub fn write_all(&mut self, mut data: &[u8]) -> Result<(), Error> {
        while !data.is_empty() {
            let n = self.write(data)?;
            if n == 0 {
                return Err(Error::NoSpace);
            }
            data = &data[n..];
        }
        Ok(())
    }

    /// Size of the file in bytes.
    pub fn size(&self) -> Result<u32, Error> {
        let rc = unsafe { abi::k32_fs_size(self.fd) };
        Error::from_rc(rc)
    }

    /// Closes the file and reports the host's result (dropping closes too, silently).
    pub fn close(self) -> Result<(), Error> {
        let fd = self.fd;
        core::mem::forget(self);
        let rc = unsafe { abi::k32_fs_close(fd) };
        Error::from_rc(rc).map(|_| ())
    }

    pub fn raw_fd(&self) -> i32 {
        self.fd
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe { abi::k32_fs_close(self.fd) };
    }
}

/// Lists `dir` (`""` for the volume root) into `out`: one `name\n` per entry,
/// directories as `name/\n`. Entries that do not fit are left out whole.
/// Returns the filled part of `out`.
pub fn list<'b>(dir: &str, out: &'b mut [u8]) -> Result<&'b [u8], Error> {
    let cap = out.len().min(MAX_IO) as u32;
    let rc = unsafe { abi::k32_fs_list(dir.as_ptr(), dir.len() as u32, out.as_mut_ptr(), cap) };
    let n = Error::from_rc(rc)? as usize;
    Ok(&out[..n.min(out.len())])
}

/// Removes a file or an empty directory.
pub fn remove(path: &str) -> Result<(), Error> {
    let rc = unsafe { abi::k32_fs_remove(path.as_ptr(), path.len() as u32) };
    Error::from_rc(rc).map(|_| ())
}
