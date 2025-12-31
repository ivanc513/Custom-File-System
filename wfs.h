#include <time.h>
#include <sys/stat.h>
#include <stdint.h>

#define BLOCK_SIZE (512)
#define MAX_NAME   (28)

#define D_BLOCK    (6)
#define IND_BLOCK  (D_BLOCK+1)
#define N_BLOCKS   (IND_BLOCK+1)

/*
  The fields in the superblock should reflect the structure of the filesystem.
  `mkfs` writes the superblock to offset 0 of the disk image. 
  The disk image will have this format:

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0    ^                   ^
i_bitmap_ptr        i_blocks_ptr

*/

// Superblock
struct wfs_sb {
    size_t num_inodes;
    size_t num_data_blocks;
    off_t i_bitmap_ptr;
    off_t d_bitmap_ptr;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
};

// Inode
// Color tag palette: stored compactly as a uint8_t enum code
typedef enum {
    WFS_COLOR_NONE = 0,
    WFS_COLOR_RED,
    WFS_COLOR_GREEN,
    WFS_COLOR_BLUE,
    WFS_COLOR_YELLOW,
    WFS_COLOR_MAGENTA,
    WFS_COLOR_CYAN,
    WFS_COLOR_WHITE,
    WFS_COLOR_BLACK,
    WFS_COLOR_ORANGE,
    WFS_COLOR_PURPLE,
    WFS_COLOR_GRAY,
    WFS_COLOR_MAX
} wfs_color_t;
struct wfs_inode {
    int     num;      /* Inode number */
    mode_t  mode;     /* File type and mode */
    uid_t   uid;      /* User ID of owner */
    gid_t   gid;      /* Group ID of owner */
    off_t   size;     /* Total size, in bytes */
    int     nlinks;   /* Number of links */
    time_t     atim;
    time_t     ctim;
    time_t     mtim;
    uint8_t color;
    off_t blocks[N_BLOCKS];
};

// Directory entry
struct wfs_dentry {
    char name[MAX_NAME];
    int num;
};

int get_inode_from_path(char* path, struct wfs_inode** inode);
char* data_offset(struct wfs_inode* inode, off_t offset, int alloc);
int add_dentry(struct wfs_inode* parent, int num, char* name);
int remove_dentry(struct wfs_inode* inode, int inum);
int dentry_to_num(char* name, struct wfs_inode* inode);
void free_block(off_t blk);
void free_inode(struct wfs_inode* inode);
struct wfs_inode* retrieve_inode(int num);
off_t allocate_data_block(void);
struct wfs_inode* allocate_inode(void);
void fillin_inode(struct wfs_inode* inode, mode_t mode);
void create_root_dir(void);

int wfs_getxattr(const char *path, const char *name, char *value, size_t size); 
