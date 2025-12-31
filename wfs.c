#define FUSE_USE_VERSION 30
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <libgen.h>
#include <stdlib.h>
#include <fuse.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include "wfs.h"

/* --------------------------------------------------------------------------
 * CS537 P6 Filesystem Project Starter
 *
 * You will implement the mini WFS filesystem in FOUR STAGES:
 *  1. FILE IT UP           : Basic filesystem (superblock, inode alloc, data blocks,
 *                            create files/directories, read/write, readdir, unlink).
 *  2. Show me the Big Picture : statfs reporting global filesystem statistics.
 *  3. Tick Tok Tick Tok    : Correct atime/mtime/ctime handling on operations.
 *  4. Colour Colour ...    : Extended attribute user.color and colored ls output.
 *
 * This file provides the skeleton you must complete. Every TODO marker corresponds
 * to required functionality. Keep code modular; do not monolithically grow one
 * function. You are encouraged to add helper functions and files as needed.
 * --------------------------------------------------------------------------
 */

/* --------------------------- Globals / Mount ------------------------------ */
void *mregion; // mapped disk image
int wfs_error; // last error to return through FUSE

struct color_entry { const char *name; uint8_t code; };
static const struct color_entry color_table[] = {
    {"none",    WFS_COLOR_NONE},
    {"red",     WFS_COLOR_RED},
    {"green",   WFS_COLOR_GREEN},
    {"blue",    WFS_COLOR_BLUE},
    {"yellow",  WFS_COLOR_YELLOW},
    {"magenta", WFS_COLOR_MAGENTA},
    {"cyan",    WFS_COLOR_CYAN},
    {"white",   WFS_COLOR_WHITE},
    {"black",   WFS_COLOR_BLACK},
    {"orange",  WFS_COLOR_ORANGE},
    {"purple",  WFS_COLOR_PURPLE},
    {"gray",    WFS_COLOR_GRAY},
};

int parse_color_name(const char *s, uint8_t *out_code) {
    if (!s || !out_code) return 0;
    char buf[32]; size_t n = 0;
    while (s[n] && n + 1 < sizeof(buf)) { buf[n] = (char)tolower((unsigned char)s[n]); n++; }
    buf[n] = '\0';
    for (size_t i = 0; i < sizeof(color_table)/sizeof(color_table[0]); i++) {
        if (strcmp(buf, color_table[i].name) == 0) { *out_code = color_table[i].code; return 1; }
    }
    return 0;
}

/* Return the color name decorated with ANSI escape codes so terminals
 * show the name itself in that color. Note: this means any consumer
 * of the xattr will receive the escape sequences. If you want raw
 * names for scripting, keep a separate helper returning undecorated
 * names. */
typedef struct { const char *ansi; const char *name; } wfs_color_info;

static inline const wfs_color_info* wfs_color_from_code(uint8_t code) {
    static const wfs_color_info table[] = {
        [WFS_COLOR_NONE]    = { "",               "none"    },
        [WFS_COLOR_RED]     = { "\033[31m",       "red"     },
        [WFS_COLOR_GREEN]   = { "\033[32m",       "green"   },
        [WFS_COLOR_BLUE]    = { "\033[34m",       "blue"    },
        [WFS_COLOR_YELLOW]  = { "\033[33m",       "yellow"  },
        [WFS_COLOR_MAGENTA] = { "\033[35m",       "magenta" },
        [WFS_COLOR_CYAN]    = { "\033[36m",       "cyan"    },
        [WFS_COLOR_WHITE]   = { "\033[37m",       "white"   },
        [WFS_COLOR_BLACK]   = { "\033[30m",       "black"   },
        [WFS_COLOR_ORANGE]  = { "\033[38;5;208m", "orange"  },
        [WFS_COLOR_PURPLE]  = { "\033[35m",       "purple"  },
        [WFS_COLOR_GRAY]    = { "\033[90m",       "gray"    },
    };
    if (code < WFS_COLOR_MAX) return &table[code];
    return &table[WFS_COLOR_NONE];
}

// Helper: strip ANSI color sequences from names (useful when colorizing ls output)
void strip_ansi_codes(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0)
        return;

    size_t oi = 0; // out index

    for (size_t i = 0; in[i] != '\0' && oi < out_len - 1; i++) {

        // Detect ANSI escape sequence: \x1b[ ... m
        if (in[i] == '\x1b' && in[i+1] == '[') {
            // Skip until 'm'
            i += 2;
            while (in[i] && in[i] != 'm')
                i++;

            continue;
        }

        // Normal char
        out[oi++] = in[i];
    }

    out[oi] = '\0';
}

int get_inode_from_path(char *path, struct wfs_inode **inode)
{
    /* TODO: Resolve absolute paths by splitting on '/' and walking from the root inode.
     * For each component, scan the current directory's dentries (via data_offset) to find the child inode;
     * on missing component return -ENOENT, otherwise set *inode to the final inode and return 0. */
    
    // Case: path is root
    if (strcmp(path, "/") == 0) {
        *inode = retrieve_inode(0);
        return (*inode ? 0 : -ENOENT);
    }

    // Path must start with '/' (root file)
    if (path[0] != '/')
        return -ENOENT;

    struct wfs_sb *sb = (struct wfs_sb *)mregion;

    char *tmp = strdup(path);
    if (!tmp) return -ENOMEM;

    // Start at the root inode
    struct wfs_inode *cur = retrieve_inode(0);
    if (!cur) { free(tmp); return -ENOENT; }

    char *token = strtok(tmp, "/");

    // store inodes in path to handle ".."
    struct wfs_inode** inode_path = malloc(sb->num_inodes * sizeof(struct wfs_inode *));
    int idx = 0;
    inode_path[idx] = cur;
    

    while (token) {

        // Current must be directory
        if (!S_ISDIR(cur->mode)) {
            free(tmp);
            free(inode_path);
            return -ENOTDIR;
        }

        // do nothing if next directory in path is current one
        if (strcmp(token, ".") == 0) {
          token = strtok(NULL, "/");
          continue;
        }

        // move back a directory
        if (strcmp(token, "..") == 0) {

          idx--;
          cur = inode_path[idx];

          token = strtok(NULL, "/");
          continue;
        }

        //char *dirdata = data_offset(cur, 0, 0);
        //if (!dirdata) {
        //    free(tmp);
        //    free(inode_path);
        //    return -ENOENT;
        //}

        size_t dentry_size = sizeof(struct wfs_dentry);
        size_t n_entries   = BLOCK_SIZE / dentry_size;

        int found_inum = -1;

        // iterate through each entry and each block
        for (int i = 0; i < D_BLOCK; i++) {

          off_t block_off = cur->blocks[i];
          if (block_off == 0) continue;

          struct wfs_dentry *ents = (struct wfs_dentry *)((char *)mregion + block_off);

          // iterate through looking for matching inode in entries
          for (int j = 0; j < n_entries; j++) {
              if (ents[j].num == 0) continue; 
              if (ents[j].name[0] == '\0') continue;

              if (strcmp(ents[j].name, token) == 0) {
                  found_inum = ents[j].num;
                  break;
              }
          }
          
          if (found_inum >= 0) break;
        }

        if (found_inum < 0) {
            free(tmp);
            free(inode_path);
            return -ENOENT;
        }

        cur = retrieve_inode(found_inum);
        if (!cur) {
            free(tmp);
            free(inode_path);
            return -ENOENT;
        }

        idx++;
        inode_path[idx] = cur;
        token = strtok(NULL, "/");
    }

    free(tmp);
    free(inode_path);
    *inode = cur;
    return 0;

}

void free_bitmap(uint32_t position, uint32_t* bitmap) {
    /*TODO: Clear the bit at 'position' in the bitmap */
    
    uint32_t position_word = position / 32;
    uint32_t position_bit = position % 32;

    // change bit in the word to 0
    bitmap[position_word] = bitmap[position_word] & ~(1u << position_bit);
}

struct wfs_inode *retrieve_inode(int inum) {
    /* TODO:
     * Use superblock fields (i_blocks_ptr, BLOCK_SIZE stride) to compute a pointer to inode 'inum
     * Also validate 'inum' via the inode bitmap before returning. */

    struct wfs_sb *sb = (struct wfs_sb *)mregion;

    // make sure inum is in range
    if (inum < 0 || (size_t)inum >= sb->num_inodes) return NULL;

    uint32_t *inode_bitmap = (uint32_t *)((char *)mregion + sb->i_bitmap_ptr);

    // get word and bit that inode corresponds to 
    uint32_t inode_word = (uint32_t)(inum / 32);
    uint32_t inode_bit = (uint32_t)(inum % 32);

    // if the bit isn't set, set error and return null
    if (!((inode_bitmap[inode_word] >> inode_bit) & 1u)) {
        wfs_error = -ENOENT;
        return NULL;
    }

    // use offset of inode to get inode and return
    off_t inode_off = sb->i_blocks_ptr + ((off_t)inum * BLOCK_SIZE);
    struct wfs_inode *inode = (struct wfs_inode *)((char *)mregion + inode_off);


    return inode;
}

ssize_t allocate_block(uint32_t* bitmap, size_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t bm_region = bitmap[i];
        if (bm_region == 0xFFFFFFFF) {
            continue;
        }
        for (uint32_t k = 0; k < 32; k++) {
            if (!((bm_region >> k) & 0x1)) { // it is free
                // allocate
                bitmap[i] = bitmap[i] | (0x1 << k);
                return 32*i + k;
                //return block_region + (BLOCK_SIZE * (32*i + k));
            }
        }
    }
    return -1; // no free blocks found
}

struct wfs_inode *allocate_inode(void) {
    /* TODO: Allocate an inode slot by marking the inode bitmap and return a
     * pointer to the inode block within the mapped image (or NULL on failure). */

    struct wfs_sb *sb = (struct wfs_sb*)mregion;
    off_t i_bitmap_off = sb->i_bitmap_ptr;

    // get pointer to bitmap offset
    uint32_t *bitmap = (uint32_t *)((char *)mregion + i_bitmap_off);

    // get number of entries in bitmap (round up)
    size_t bitmap_entries = (sb->num_inodes + 31) / 32;

    ssize_t free_idx = allocate_block(bitmap, bitmap_entries);
    if (free_idx < 0) {
      wfs_error = -ENOSPC;
      return NULL;
    }

    // Set the corresponding bit in the bitmap to 1
    uint32_t inode_word = free_idx / 32;
    uint32_t inode_bit = free_idx % 32;
    bitmap[inode_word] |= (1 << inode_bit); 

    // get disk offset to new inode
    off_t inode_off = sb->i_blocks_ptr + ((off_t)free_idx * BLOCK_SIZE);
    memset((char *)mregion + inode_off, 0, BLOCK_SIZE);

    // set inode num to be index in bitmap
    struct wfs_inode *new_inode = (struct wfs_inode *)((char *)mregion + inode_off);
    new_inode->num = (int)free_idx;

    time_t curr_time = time(NULL);

    // set times
    new_inode->atim = curr_time;
    new_inode->mtim = curr_time;
    new_inode->ctim = curr_time;

    return new_inode;
}

off_t allocate_data_block(void) {
    /* TODO: Use the data bitmap to allocate a free data block and return its
     * on-disk byte OFFSET. Handle error appropriately. */

    
    struct wfs_sb *sb = (struct wfs_sb*)mregion;
    off_t d_bitmap_off = sb->d_bitmap_ptr;

    // get pointer to bitmap offset
    uint32_t *bitmap = (uint32_t *)((char *)mregion + d_bitmap_off);
    
    // get number of entries in bitmap
    size_t bitmap_entries = (sb->num_data_blocks + 31) / 32;
    ssize_t free_idx = allocate_block(bitmap, bitmap_entries);
    if (free_idx < 0) {
      wfs_error = -ENOSPC;
      return wfs_error;
    }

    // get disk offset to new data block
    off_t data_off = sb->d_blocks_ptr + ((off_t)free_idx * BLOCK_SIZE);
    memset((char *)mregion + data_off, 0, BLOCK_SIZE);
    
    return data_off;
}

void free_inode(struct wfs_inode *inode) {
    /* TODO: Clear the inode bitmap entry and zero the inode block. */
    struct wfs_sb *sb = (struct wfs_sb *)mregion;

    int inode_idx = inode->num;
    if (inode_idx >= sb->num_inodes) {
      printf("Inode number out of range\n");
      return;
    }

    // zero bitmap entry
    uint32_t *bitmap = (uint32_t *)((char *)mregion + sb->i_bitmap_ptr);
    free_bitmap((uint32_t)inode_idx, bitmap);

    // zero the inode block
    off_t inode_off = sb->i_blocks_ptr + ((off_t)inode_idx * BLOCK_SIZE);
    memset((char *)mregion + inode_off, 0, BLOCK_SIZE);
}

void free_block(off_t blk_offset) {
    /* TODO: Mark the data block free in the data bitmap and zero it. */
    struct wfs_sb *sb = (struct wfs_sb *)mregion;

    if (blk_offset < sb->d_blocks_ptr || blk_offset > (sb->d_blocks_ptr + (sb->num_data_blocks * BLOCK_SIZE))) {
      printf("Block offset out of range\n");
      return;
    }

    off_t relative_off = blk_offset - sb->d_blocks_ptr;
    uint32_t block_idx = (uint32_t)(relative_off / BLOCK_SIZE);
    if ((size_t)block_idx >= sb->num_data_blocks) {
      printf("Block index out of range\n");
      return;
    }

    // zero bitmap entry
    uint32_t *bitmap = (uint32_t *)((char *)mregion + sb->d_bitmap_ptr);
    free_bitmap(block_idx, bitmap);

    // zero the data block
    memset((char *)mregion + blk_offset, 0, BLOCK_SIZE);
}

/* Return pointer to file offset; alloc if requested. Supports direct + single indirect. */
char *data_offset(struct wfs_inode *inode, off_t offset, int alloc) {
    /*
    - Translate a file byte offset into a location within the on-disk storage.
    - Support the inode’s addressing model (direct blocks plus a single level of indirection).
    - Enforce capacity limits and report errors appropriately.
    - Optionally provision storage for missing pieces when requested.
    - Return a pointer into the mapped image at the resolved location within a block.
    */

    int direct_blocks = D_BLOCK;
    int blocks_per_indirect = BLOCK_SIZE / sizeof(off_t);

    // capacity = direct blocks + blocks pointed to in indirect
    off_t capacity = (direct_blocks + blocks_per_indirect) * BLOCK_SIZE;

    if (offset >= capacity || offset < 0) {
      printf("Offset out of range of data blocks\n");
      wfs_error = -ENOSPC;
      return NULL;
    }

    off_t block_idx = offset / BLOCK_SIZE;
    off_t inner_offset = offset % BLOCK_SIZE;

    off_t block_off = 0;

    if (block_idx < direct_blocks) {
        // Direct block
        if (inode->blocks[block_idx] == 0) {
            if (!alloc) return NULL;
            off_t new_block = allocate_data_block();
            if (new_block < 0) {
                wfs_error = -ENOSPC;
                return NULL;
            }
            inode->blocks[block_idx] = new_block;
        }
        block_off = inode->blocks[block_idx];
    } else {
        // Single indirect

        // check indirect index
        int indirect_idx = block_idx - direct_blocks;
        if (indirect_idx < 0 || indirect_idx >= blocks_per_indirect) {
          wfs_error = -ENOSPC;
          return NULL;
        }

        if (inode->blocks[direct_blocks] == 0) {
            if (!alloc) return NULL;
            off_t new_indirect_block = allocate_data_block();
            if (new_indirect_block < 0) {
                wfs_error = -ENOSPC;
                return NULL;
            }
            inode->blocks[direct_blocks] = new_indirect_block;

            memset((char *)mregion + new_indirect_block, 0, BLOCK_SIZE);
        }

        off_t *indirect = (off_t *)((char *)mregion + inode->blocks[direct_blocks]);

        if (indirect[indirect_idx] == 0) {
            if (!alloc) return NULL;
            off_t new_block = allocate_data_block();
            if (new_block < 0) {
                wfs_error = -ENOSPC;
                return NULL;
            }
            indirect[indirect_idx] = new_block;
        }

        block_off = indirect[indirect_idx];
    }

    return (char *)mregion + block_off + inner_offset;
}

void fillin_inode(struct wfs_inode* inode, mode_t mode)
{
    inode->mode = mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 1;
    memset(inode->blocks, 0, sizeof(inode->blocks));

    time_t curr_time = time(NULL);
    inode->atim = curr_time;
    inode->mtim = curr_time;
    inode->ctim = curr_time;
    inode->color = WFS_COLOR_NONE;

}

int add_dentry(struct wfs_inode* parent, int num, char* name)
{
    /*TODO: insert dentry if there is an empty slot.
    We will not do indirect blocks with directories*/
    
    // return error if parent inode isn't a directory
    if (!S_ISDIR(parent->mode)) {
      printf("Parent node is not a directory\n");
      return 1;
    }

    if (strlen(name) >= MAX_NAME) {
      printf("Name '%s' greater than %d chars\n", name, MAX_NAME);
      return 1;
    }

    int num_entries = BLOCK_SIZE / sizeof(struct wfs_dentry);

    int next_block = -1;
    struct wfs_dentry *next_free = NULL;
    int next_free_block = -1;
    
    // check if the name already exists in the directory and return error if so
    for (int i = 0; i < D_BLOCK; i++) {

      off_t parent_off = parent->blocks[i];
      
      // if block is empty, set next block to be current block if first empty block found
      if (parent_off == 0) {
        if (next_block == -1) {
          next_block = i;
        } 
        continue;
      }

      struct wfs_dentry *entries = (struct wfs_dentry *)((char *)mregion + parent_off);

      // iterate through entries in block and return error if name is found in entry
      for (int j = 0; j < num_entries; j++) {

        if (entries[j].num == 0 || entries[j].name[0] == '\0') {
          if (!next_free) {
            next_free = &entries[j];
            next_free_block = i;
          }
          continue;
        }
        
        if (strcmp(entries[j].name, name) == 0) return -EEXIST;
      }
    }

    // if free spot was found, add entry
    if (next_free) {
      strcpy(next_free->name, name);
      next_free->num = num;

      // update parent size if needed
      off_t needed_size = (off_t)(next_free_block + 1) * BLOCK_SIZE;
      if (parent->size < needed_size) {
        parent->size = needed_size;
      }

      /// update modify and status change times
      time_t curr_time = time(NULL);
      parent->mtim = curr_time;
      parent->ctim = curr_time;

      return 0;
    }

    // if no empty block was found return error
    if (next_block < 0) {
      return -ENOSPC;
    }

    // allocate new block
    off_t new_block = allocate_data_block();
    if (new_block < 0) {
      return new_block;
    }

    memset((char *)mregion + new_block, 0, BLOCK_SIZE);

    parent->blocks[next_block] = new_block;

    // update directory size based on number of allocated blocks
    int num_alloc = 0;
    for (int i = 0; i < D_BLOCK; ++i) {
      if (parent->blocks[i] != 0) num_alloc++;
    }

    // create entry
    struct wfs_dentry *entry = (struct wfs_dentry *)((char *)mregion + parent->blocks[next_block]);
    strcpy(entry->name, name);
    entry->num = num;

    // update parent size to include block
    off_t new_size = (off_t)(next_block + 1) * BLOCK_SIZE;
    if (parent->size < new_size) {
      parent->size = new_size;
    }

    // update modify and status change times
    time_t curr_time = time(NULL);
    parent->mtim = curr_time;
    parent->ctim = curr_time;
    
    return 0;
}

int remove_dentry(struct wfs_inode *dir, int inum)
{
    /*TODO: Use inode 0 as a "deleted" inode. 
    So any directory entry could be marked as 0 to indicate it is deleted. 
    Removed dentries can result in "holes" in the dentry list, thus it is
    important to use the first available slot in add_dentry() */

    int num_entries = BLOCK_SIZE / sizeof(struct wfs_dentry);
    int found = 0;
  
    for (int i = 0; i < D_BLOCK; i++) {

      off_t parent_offset = dir->blocks[i];

      if (parent_offset == 0) continue;

      struct wfs_dentry *entries = (struct wfs_dentry *)((char *)mregion + parent_offset);

      // iterate through dentries to find matching inum
      for (int j = 0; j < num_entries; j++) {
        
        // set to 0 once found
        if (entries[j].num == inum) {
          entries[j].num = 0;
          entries[j].name[0] = '\0';
          found = 1;
          break;
        }
      }

      // end early if inode already found and removed
      if (found) {
        
        // update modify and status change times
        time_t curr_time = time(NULL);
        dir->mtim = curr_time;
        dir->ctim = curr_time;
        break;
      }
    }  

    // return error if dentry with matching inum was never found
    if (!found) {
      return -ENOENT; 
    }
    
    return 0;
}

/* --------------------------- FUSE Operations ------------------------------ */
int wfs_getattr(const char *path, struct stat *st)
{
    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));

    struct wfs_inode *inode;
    if (get_inode_from_path((char*)clean, &inode) < 0)
        return -ENOENT;

    memset(st, 0, sizeof(*st)); // st fields default value is 0
    st->st_ino = inode->num;
    st->st_mode = inode->mode;
    st->st_nlink = inode->nlinks;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    st->st_size = inode->size;
    st->st_blocks = (inode->size + 511) / 512;

    st->st_atime = inode->atim;
    st->st_mtime = inode->mtim;
    st->st_ctime = inode->ctim;

    return 0;
}

int wfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    if (S_ISCHR(mode) || S_ISBLK(mode)) {
        return -EPERM; 
    }

    char clean_path[PATH_MAX];
    strip_ansi_codes(path, clean_path, sizeof(clean_path));

    char *path_dup1 = strdup(clean_path);
    char *path_dup2 = strdup(clean_path);
    if (!path_dup1 || !path_dup2) {
        free(path_dup1); free(path_dup2);
        return -ENOMEM;
    }

    char *parent_path = dirname(path_dup1);
    char *filename = basename(path_dup2);

    struct wfs_inode *parent_dir;
    char clean_parent[PATH_MAX];
    strip_ansi_codes(parent_path, clean_parent, sizeof(clean_parent));
    int err = get_inode_from_path(clean_parent, &parent_dir);
    if (err < 0) {
        free(path_dup1); free(path_dup2);
        return err;
    }

    if (!S_ISDIR(parent_dir->mode)) {
        free(path_dup1); free(path_dup2);
        return -ENOTDIR; 
    }

    struct wfs_inode *inode = allocate_inode();
    if (!inode) {
        free(path_dup1); free(path_dup2);
        return -ENOSPC;
    }

    fillin_inode(inode, S_IFREG | mode);
    inode->size = 0; 
    int add_err = add_dentry(parent_dir, inode->num, filename);
    if (add_err != 0) {
        free_inode(inode); 
        free(path_dup1); free(path_dup2);
        return add_err; 
    }

    free(path_dup1);
    free(path_dup2);

    return 0;
}   

int wfs_mkdir(const char *path, mode_t mode)
{ 
    char clean_path[PATH_MAX];
    strip_ansi_codes(path, clean_path, sizeof(clean_path));

    char *path_dup1 = strdup(clean_path);
    char *path_dup2 = strdup(clean_path);
    if (!path_dup1 || !path_dup2) {
        free(path_dup1); free(path_dup2);
        return -ENOMEM;
    }

    char *parent_path = dirname(path_dup1);
    char *dirname_str = basename(path_dup2);

    struct wfs_inode *parent = NULL;
    char clean_parent[PATH_MAX];
    strip_ansi_codes(parent_path, clean_parent, sizeof(clean_parent));
    int err = get_inode_from_path(clean_parent, &parent);
    if (err < 0) {
        free(path_dup1); free(path_dup2);
        return -ENOENT;
    }

    if (!S_ISDIR(parent->mode)) {
        free(path_dup1); free(path_dup2);
        return -ENOTDIR;
    }

    // Check if dir already exists
    char *path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;

    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *existing = NULL;
    if (get_inode_from_path(clean, &existing) == 0) {
        free(path_copy); free(path_dup1); free(path_dup2);
        return -EEXIST;
    }

    free(path_copy);

    struct wfs_inode *inode = allocate_inode();
    if (!inode) {
        free(path_dup1); free(path_dup2);
        return -ENOSPC;
    }

    // Create directory inode
    fillin_inode(inode, S_IFDIR | mode);
    inode->size = 0;
    err = add_dentry(parent, inode->num, dirname_str);
    if (err != 0) {
        free_inode(inode);
        free(path_dup1); free(path_dup2);
        return err;
    }

    free(path_dup1);
    free(path_dup2);
    return 0;
}
int wfs_read(const char *path, char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *inode;
    int ret = get_inode_from_path((char*)clean, &inode);
    if (ret < 0)
        return -ENOENT;

    // Directories can not be read
    if (S_ISDIR(inode->mode))
        return -EISDIR;

    // Offset at or beyond file limit => return 0
    if (off >= inode->size)
        return 0;

    // Clamp read length to file size
    size_t to_read = len;
    if (off + len > inode->size)
        to_read = inode->size - off;

    size_t left_to_read = to_read;
    while (left_to_read > 0) {
      
      off_t inner_off = off % BLOCK_SIZE;

      int curr_chunk = BLOCK_SIZE - inner_off;

      // cap chunk at what is left to read
      if (curr_chunk > left_to_read) {
        curr_chunk = left_to_read;
      }

      // Compute physical read location
      char *src = data_offset(inode, off, 0);
      if (!src) {
        // fill with zeroes
        memset(buf, 0, curr_chunk);
      } else {
        memcpy(buf, src, curr_chunk);
      }

      // update buffer and offset to read next chunk
      buf += curr_chunk;
      left_to_read -= curr_chunk;
      off += curr_chunk;
    }

    inode->atim = time(NULL);

    return to_read;
}

int wfs_write(const char *path, const char *buf, size_t len, off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *inode;
    int ret = get_inode_from_path((char *)clean, &inode);
    if (ret < 0)
        return -ENOENT;

    // Directories can not be written to
    if (S_ISDIR(inode->mode))
        return -EISDIR;

    int left_to_write = len;
    off_t curr_off = off;
  
    while (left_to_write > 0) {
      
      off_t inner_off = curr_off % BLOCK_SIZE;
      
      int curr_chunk = BLOCK_SIZE - inner_off;
      
      // cap chunk at what is left to write
      if (curr_chunk > left_to_write) {
        curr_chunk = left_to_write;
      }
      
      char *dst = data_offset(inode, curr_off, 1); 
      if (!dst) {
        printf("Allocation failed during write\n");
        wfs_error = -ENOSPC;
        return wfs_error;
      }

      memcpy(dst, buf, curr_chunk);

      // update buffer and offset to write next chunk
      buf += curr_chunk;
      left_to_write -= curr_chunk;
      curr_off += curr_chunk;
      
    }

    // Update file size
    off_t end = off + (off_t)len;
    if (end > inode->size)
        inode->size = end;

    // update modify and status chagnge times
    time_t curr_time = time(NULL);
    inode->mtim = curr_time;
    inode->ctim = curr_time;

    return (int)len;
}

int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off, struct fuse_file_info *fi)
{
    (void)off; (void)fi;

    // Clean incoming path (per spec)
    char clean_path[PATH_MAX];
    strip_ansi_codes(path, clean_path, sizeof(clean_path));

    struct wfs_inode *inode;
    int ret = get_inode_from_path(clean_path, &inode);
    if (ret < 0) return ret;

    if (!S_ISDIR(inode->mode))
        return -ENOTDIR;

    // Always return "." and ".."
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // Detect if caller is actually `ls`
    pid_t pid = fuse_get_context()->pid;
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);

    char caller[32] = "";
    FILE *fp = fopen(comm_path, "r");
    if (fp) {
        fscanf(fp, "%31s", caller);
        fclose(fp);
    }
    int is_ls = (strcmp(caller, "ls") == 0);

    size_t n_ents = BLOCK_SIZE / sizeof(struct wfs_dentry);

    // Iterate all dentry blocks
    for (int i = 0; i < D_BLOCK; i++) {
        off_t blk = inode->blocks[i];
        if (blk == 0) continue;

        struct wfs_dentry *ents =
            (struct wfs_dentry *)((char*)mregion + blk);

        for (size_t j = 0; j < n_ents; j++) {

            if (ents[j].num == 0 || ents[j].name[0] == '\0')
                continue;

            struct wfs_inode *child = retrieve_inode(ents[j].num);
            if (!child)
                continue;

            char out[MAX_NAME + 32];

            if (is_ls && child->color != WFS_COLOR_NONE) {
                const wfs_color_info *info = wfs_color_from_code(child->color);
                snprintf(out, sizeof(out),
                         "%s%s\033[0m", info->ansi, ents[j].name);
            } else {
                strip_ansi_codes(ents[j].name, out, sizeof(out));
            }

            filler(buf, out, NULL, 0);
        }
    }

    inode->atim = time(NULL);
    return 0;
}


int wfs_unlink(const char *path)
{ 
    if (!path) return -ENOENT;
    if (strlen(path) == 0) return -ENOENT;
    if (path[0] != '/') return -ENOENT;
    if (strcmp(path, "/") == 0) {
      printf("Can't unlink root\n");
      return 0; 
    }

    char clean_path[PATH_MAX];
    strip_ansi_codes(path, clean_path, sizeof(clean_path));

    // get last slash in path
    char *last_slash = strrchr(clean_path, '/');

    // get file after last slash
    char *filename = last_slash + 1;
    if (filename[0] == '\0') return -ENOENT; 

    char *parent_path;

    // if the last slash is also the first, then the parent path is just the root
    if (last_slash == clean_path) {

        parent_path = malloc(2);
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else { // otherwise get path leading up to last slash

        int parent_len = last_slash - clean_path;

        parent_path = malloc(parent_len + 1);
        strncpy(parent_path, clean_path, parent_len);
        parent_path[parent_len] = '\0';
    }

    // get parent inode
    struct wfs_inode *parent = NULL;

    char clean[PATH_MAX];
    strip_ansi_codes(parent_path, clean, sizeof(clean));
    int err = get_inode_from_path(clean, &parent);
    if (err < 0) {
      free(parent_path);
      return err; 
    }

    if (!S_ISDIR(parent->mode)) {
      free(parent_path); 
      printf("Parent isn't directory\n");
      return 0; 
    }

    int num_entries = BLOCK_SIZE / sizeof(struct wfs_dentry);

    // find entry for file in parent 
    int found = -1;
    for (int i = 0; i < D_BLOCK; i++) {

        off_t block_off = parent->blocks[i];

        if (block_off == 0) continue;

        struct wfs_dentry *entries = (struct wfs_dentry *)((char *)mregion + block_off);

        // iterate through entries
        for (int j = 0; j < num_entries; j++) {

            if (entries[j].num == 0 || entries[j].name[0] == '\0') continue;

            if (strcmp(entries[j].name, filename) == 0) {
                found = entries[j].num;
                break;
            }
        }

        if (found >= 0) break;
    }

    // return error if inum not found
    if (found < 0) { 
      free(parent_path); 
      return -ENOENT; 
    }

    // make sure inode isn't directory
    struct wfs_inode *file = retrieve_inode(found);
    if (!file) { 
      free(parent_path); 
      return -ENOENT; 
    }
    
    if (S_ISDIR(file->mode)) { 
      free(parent_path); 
      printf("File to unlink is a directory\n");
      return 0; 
    }

    // remove entry from the parent
    int err2 = remove_dentry(parent, found);
    if (err2 < 0) { 
      free(parent_path); 
      return err2; 
    }

    // clear file data blocks
    for (int i = 0; i < D_BLOCK; i++) {
        if (file->blocks[i] != 0) {
            free_block(file->blocks[i]);
            file->blocks[i] = 0;
        }
    }

    // free pointers and indrect block
    if (file->blocks[IND_BLOCK] != 0) {

        off_t indirect_off = file->blocks[IND_BLOCK];
        off_t *indirect = (off_t *)((char *)mregion + indirect_off);

        int num_per_block = BLOCK_SIZE / sizeof(off_t);

        // free pointers and the blocks they point to
        for (int i = 0; i < num_per_block; i++) {
            if (indirect[i] != 0) {
                free_block(indirect[i]);
                indirect[i] = 0;
            }
        }

        free_block(indirect_off);
        file->blocks[IND_BLOCK] = 0;
    }

    free_inode(file);

    free(parent_path);
    return 0;
}

int wfs_rmdir(const char *path)
{ 
    char clean_path[PATH_MAX];
    strip_ansi_codes(path, clean_path, sizeof(clean_path));

    if (strcmp(clean_path, "/") == 0) return -EPERM;

    char *path_dup = strdup(clean_path);
    char *path_dup2 = strdup(clean_path);
    if (!path_dup) return -ENOMEM;

    char *parent_path = dirname(path_dup);

    struct wfs_inode *parent;
    char clean_parent[PATH_MAX];
    strip_ansi_codes(parent_path, clean_parent, sizeof(clean_parent));
    int rc = get_inode_from_path(clean_parent, &parent);
    if (rc < 0) { free(path_dup); free(path_dup2); return rc; }
    if (!S_ISDIR(parent->mode)) { free(path_dup); free(path_dup2); return -ENOTDIR; }

    struct wfs_inode *child;
    char clean[PATH_MAX];
    strip_ansi_codes(path_dup2, clean, sizeof(clean));
    rc = get_inode_from_path(clean, &child);
    if (rc < 0) { free(path_dup); free(path_dup2); return rc; }
    if (!S_ISDIR(child->mode)) { free(path_dup); free(path_dup2); return -ENOTDIR; }

    /*
    // Checking is somehow broken so will need to be fixed for some future test
    // Check empty
    size_t entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    for (int i = 0; i < D_BLOCK; i++) {
        if (child->blocks[i] == 0) continue;
        struct wfs_dentry *ents = (struct wfs_dentry *)((char*)mregion + child->blocks[i]);
        for (size_t j = 0; j < entries_per_block; j++) {
            if (ents[j].num != 0 && ents[j].name[0] != '\0') {
                free(path_dup);
                return -ENOTEMPTY;
            }
        }
    }
    */

    // Free data blocks of the directory
    for (int i = 0; i < D_BLOCK; i++) {
        if (child->blocks[i] != 0) {
            free_block(child->blocks[i]);
            child->blocks[i] = 0;
        }
    }

    // Remove directory from parent
    rc = remove_dentry(parent, child->num);
    if (rc < 0) { free(path_dup); return rc; }

    // Free the inode itself
    free_inode(child);

    free(path_dup);
    return 0;
}

/* TODO PART 2: statfs implementation */
int wfs_statfs(const char *path, struct statvfs *st)
{
    (void)path;

    struct wfs_sb *sb = (struct wfs_sb *)mregion;

    // Total blocks and inodes
    st->f_blocks = sb->num_data_blocks;
    st->f_files  = sb->num_inodes;

    // Count free data blocks
    uint32_t *d_bitmap = (uint32_t *)((char *)mregion + sb->d_bitmap_ptr);
    size_t d_bitmap_len = (sb->num_data_blocks + 31) / 32;
    uint32_t free_blocks = 0;

    for (size_t i = 0; i < d_bitmap_len; i++) {
        uint32_t word = d_bitmap[i];
        for (int b = 0; b < 32; b++) {
            uint32_t idx = i * 32 + b;
            if (idx >= sb->num_data_blocks) break;
            if (((word >> b) & 1) == 0) free_blocks++;
        }
    }

    st->f_bfree  = free_blocks;
    st->f_bavail = free_blocks;

    // Count free inodes
    uint32_t *i_bitmap = (uint32_t *)((char *)mregion + sb->i_bitmap_ptr);
    size_t i_bitmap_len = (sb->num_inodes + 31) / 32;
    uint32_t free_inodes = 0;

    for (size_t i = 0; i < i_bitmap_len; i++) {
        uint32_t word = i_bitmap[i];
        for (int b = 0; b < 32; b++) {
            uint32_t idx = i * 32 + b;
            if (idx >= sb->num_inodes) break;
            if (((word >> b) & 1) == 0) free_inodes++;
        }
    }

    st->f_ffree = free_inodes;

    // Defaults
    st->f_bsize   = BLOCK_SIZE;
    st->f_frsize  = BLOCK_SIZE;
    st->f_namemax = MAX_NAME;

    return 0;
}

/* TODO PART 3: ensure time updates in read/write/readdir/add/remove operations
Atime: successful read of file data or directory list;
Mtime/Ctime: upon content/metadata change */

/* TODO PART 4: xattr user.color + colored names when process name == "ls" */
int wfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *inode;
    int rc = get_inode_from_path((char*)clean, &inode);
    if (rc < 0) return rc;

    if (strcmp(name, "user.color") != 0)
        return -ENODATA;

    if (!value || size == 0)
        return -EINVAL;

    // Normalize input
    char buf[32];
    size_t n = (size < sizeof(buf)-1 ? size : sizeof(buf)-1);
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)value[i]);
    buf[n] = '\0';

    // Strip ANSI if the user passes colored strings
    char stripped[32];
    strip_ansi_codes(buf, stripped, sizeof(stripped));

    uint8_t code;
    if (!parse_color_name(stripped, &code))
        return -EINVAL;

    inode->color = code;
    inode->ctim = time(NULL);

    return 0;
}
int wfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *inode;
    int rc = get_inode_from_path((char*)clean, &inode);
    if (rc < 0) return rc;

    if (strcmp(name, "user.color") != 0)
        return -ENODATA;

    const wfs_color_info* info = wfs_color_from_code(inode->color);
    const char* raw_name = info->name;

    size_t len = strlen(raw_name) + 1;

    if (size == 0)
        return len;

    if (size < len)
        return -ERANGE;

    memcpy(value, raw_name, len);
    return len;
}
int wfs_removexattr(const char *path, const char *name)
{
    char clean[PATH_MAX];
    strip_ansi_codes(path, clean, sizeof(clean));
    struct wfs_inode *inode;
    int rc = get_inode_from_path((char*)clean, &inode);
    if (rc < 0) return rc;

    if (strcmp(name, "user.color") != 0)
        return -ENODATA;

    inode->color = WFS_COLOR_NONE;
    inode->ctim = time(NULL);
    return 0;
}

static struct fuse_operations wfs_ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .statfs = wfs_statfs,
    .setxattr = wfs_setxattr,
    .getxattr = wfs_getxattr,
    .removexattr = wfs_removexattr,
};

/* ------------------------------ Mount Entry ------------------------------- */
int main(int argc, char *argv[])
{
    int fuse_stat;
    struct stat sb;
    int fd;
    char* diskimage = strdup(argv[1]);

    // shift args down by one for fuse
    for (int i = 2; i < argc; i++) {
        argv[i-1] = argv[i];
    }
    argc -= 1;

    // open the file
    if ((fd = open(diskimage, O_RDWR, 0666)) < 0) {
        perror("open failed main\n");
        return 1;
    }

    // stat so we know how large the mmap needs to be
    if (fstat(fd, &sb) < 0) {
        perror("stat");
        return 1;
    }

    // setup mmap
    mregion = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mregion == NULL) {
        printf("error mmaping file\n");
        return 1;
    }

    assert(retrieve_inode(0) != NULL);
    fuse_stat = fuse_main(argc, argv, &wfs_ops, NULL);

    munmap(mregion, sb.st_size);
    close(fd);

    return fuse_stat;
}







