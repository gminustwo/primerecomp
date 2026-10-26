// What runtime/platform/posix.c gives runtime/platform/macos.c and linux.c beyond runtime/platform.h.
#pragma once

// stdout / stderr into primerecomp.log when started without a terminal; data_dir: this OS's default data folder
// (runtime/paths.c picks the same one)
void posix_log_to_file(const char *data_dir);
