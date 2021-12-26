#ifndef ___SERTUP_H
#define ___SETUP_H 0

#include <types.h>
#include <efi.h>
#include <disk.h>
#include <memory.h>

void video_clear_screen();
size_t video_printf(char_t* fmt, ...);
#define printf(...) video_printf(__VA_ARGS__)

disk_t* efi_disk_impl_open(efi_block_io_t* bio);

#endif