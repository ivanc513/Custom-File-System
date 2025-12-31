# Custom File System

## Overview
Project assignment from CS537 - Operating Systems at UW-Madison

## Objective
"To understand how filesystem operations are implemented.
To implement a traditional block-based filesystem.
To learn to build a user-level filesystem using FUSE.
To learn how to add new capabilities to existing filesystem paradigms."

## Features
- Basic File System (Super, inodes, data blocks)
- File System Operations (create files/dirs, read/write, readdir, link/unlink)
- File Metadata including file color mapping and timestamps

## Architecture / Design
A block-based user space file system utilizing superblocks, inodes, and data blocks
architecture. Incorporated with the use of FUSE and mounting virtual disks.
Further encapsulates metadata as structs placed in diagram seen below.

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0    ^                   ^
i_bitmap_ptr        i_blocks_ptr


## Key Concepts
Concepts demonstrated in this project:
- Basic File System layout
- File System API
- File Structs Metadata

## Build & Run
$ make
$ ./create_disk.sh                 
# # This creates a 1MB file named disk.img
$ ./mkfs -d disk.img -i 32 -b 200  
# # This initializes disk.img with 32 inodes and 200 data blocks
$ mkdir mnt
$ ./wfs disk.img -f -s mnt         
# This mounts your WFS implementation on the 'mnt' directory.
# -f runs FUSE in the foreground (so you can see printf logs)
# -s runs in single-threaded mode (required)

Then another terminal you may interact with the filesystem once mounted:
$ ls mnt
