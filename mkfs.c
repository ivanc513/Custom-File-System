#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "wfs.h"

int roundup(int num, int factor) {
    return num % factor == 0 ? num : num + (factor - (num % factor));
}

int setup_sb(struct wfs_sb* sb, int inodes, int blocks, size_t sz) {
    inodes = roundup(inodes, 32);
    blocks = roundup(blocks, 32);
    
    sb->num_inodes = inodes;
    sb->num_data_blocks = blocks;
    sb->i_bitmap_ptr = sizeof(struct wfs_sb);
    // 8 bits in a byte...
    sb->d_bitmap_ptr = sb->i_bitmap_ptr + (inodes / 8);
    sb->i_blocks_ptr = sb->d_bitmap_ptr + (blocks / 8);
    sb->d_blocks_ptr = sb->i_blocks_ptr + (inodes * BLOCK_SIZE);

    printf("trying to create with %d inodes, %d blocks, size is %ld, block start at %ld\n", inodes, blocks, sz, sb->i_blocks_ptr);
    return (inodes * BLOCK_SIZE) + (blocks * BLOCK_SIZE) + sb->i_blocks_ptr < sz;
}

// Setup superblock for disk img. 
int wfs_mkfs(char* path, int inodes, int blocks) {
    int fd;
    struct stat statb;
    struct wfs_sb sb;

    if ((fd = open(path, O_RDWR, S_IRWXU)) < 0) {
        perror("open failed create metadata\n");
        return -1;
    }

    if (fstat(fd, &statb) < 0) {
        perror("stat-ing diskimg\n");
        return -1;
    }

    if (setup_sb(&sb, inodes, blocks, statb.st_size) == 0) {
        printf("too many blocks requested, failed to write superblock\n");
        close(fd);
        return -1;
    }

    if (write(fd, &sb, sizeof(struct wfs_sb)) < 0) {
        perror("writing superblock\n");
        return -1;
    }

    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    
    struct wfs_inode inode;
    memset(&inode, 0, sizeof(struct wfs_inode));
    inode.mode = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.size = 0;
    inode.nlinks = 1;
    //TODO Initialize additional inode fields

    // set bitmap
    uint32_t bit = 0x1;
    lseek(fd, sb.i_bitmap_ptr, SEEK_SET);
    write(fd, &bit, sizeof(uint32_t));

    // write inode
    lseek(fd, sb.i_blocks_ptr, SEEK_SET);
    if (write(fd, &inode, sizeof(struct wfs_inode)) < 0) {
        perror("writing root inode\n");
        return -1;
    }
    
    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    char* diskimg;
    int inodes, blocks;
    int opt;
    
    while ((opt = getopt(argc, argv, "d:i:b:")) != -1) {
        switch (opt) {
        case 'd':
            diskimg = optarg;
            break;
        case 'i':
            inodes = atoi(optarg);
            break;
        case 'b':
            blocks = atoi(optarg);
            break;
        default:
            printf("usage: ./mkfs -d <disk img> -i <num inodes> -b <num data blocks>\n");
            exit(1);
        }
    }
    
    return wfs_mkfs(diskimg, inodes, blocks);
}
