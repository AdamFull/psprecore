// The nx VFS ZIP device needs minizip-ng, which this project does not use: the factories report failure and the
// writer refuses to open. (Replaces core/foundation/vfs/zip_device.cpp.)

#include "core/foundation/vfs/vfs_internal.h"

namespace nx::vfs {

io_result<Device *> make_zip_device(path_view, path_view) {
  return err(file_error{io_error::OpenFailed, 0});
}

io_result<Device *> make_zip_device(nx::blob<u8>, path_view) {
  return err(file_error{io_error::OpenFailed, 0});
}

io_result<Device *> make_zip_device(ZipStreamFactory, path_view) {
  return err(file_error{io_error::OpenFailed, 0});
}

ZipWriter::~ZipWriter() { m_handle = nullptr; }

bool ZipWriter::open(path_view) { return false; }

io_result<void> ZipWriter::add(path_view, std::span<const u8>, ZipCompression) const {
  return err(file_error{io_error::InvalidArgument, 0});
}

io_result<void> ZipWriter::close() { return {}; }

}  // namespace nx::vfs
