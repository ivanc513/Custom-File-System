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
Further encapsulates metadata as structs placed in SB->imap->blocks format.


## Key Concepts
Concepts demonstrated in this project:
- Basic File System layout
- File System API
- File Structs Metadata

## Build & Run
$ make
$ ./create_disk.sh 
$ ./mkfs -d disk.img -i 32 -b 200  
$ mkdir mnt
$ ./wfs disk.img -f -s mnt         

Then another terminal you may interact with the filesystem once mounted:
$ ls mnt
