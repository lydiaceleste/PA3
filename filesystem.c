#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "softwaredisk.h"
#include "filesystem.h"

FSError fserror = FS_NONE;

#define MAX_FILES 512
#define DATA_BITMAP_BLOCK 0
#define INODE_BITMAP_BLOCK 1
#define FIRST_INODE_BLOCK 2
#define LAST_INODE_BLOCK 5  //128 inodes per block, max of 4*128 = 512 inodes, thus 512 files

#define INODES_PER_BLOCK 128
#define FIRST_DIR_ENTRY_BLOCK 6
#define LAST_DIR_ENTRY_BLOCK 69
#define DIR_ENTRIES_PER_BLOCK 8 //8 DE per block, max of 64*8 = 512 directory entries
                                //corresponding to max file for single-level dir structure

#define FIRST_DATA_BLOCK 70
#define LAST_DATA_BLOCK 4095
#define MAX_FILENAME_SIZE 507
#define NUM_DIRECT_INODE_BLOCKS 13 // data blocks the innodes map to
#define NUM_SINGLE_INDIRECT_INODE_BLOCKS (SOFTWARE_DISK_BLOCK_SIZE / sizeof(uint16_t))

#define MAX_FILE_SIZE (NUM_DIRECT_INODE_BLOCKS + NUM_SINGLE_INDIRECT_INODE_BLOCKS) * SOFTWARE_DISK_BLOCK_SIZE

//struct for indirect block
typedef struct IndirectBlock {
    uint16_t blocks[NUM_SINGLE_INDIRECT_INODE_BLOCKS];
} IndirectBlock;

//struct for inode
typedef struct Inode {
    uint32_t file_size;                              //file size
    uint16_t blocks[NUM_DIRECT_INODE_BLOCKS+1]; //direct blocks + the indirect
} Inode;

//struct for a block on inodes
typedef struct InodeBlock {
    Inode inodes[INODES_PER_BLOCK];     //each inode block holds 128 inodes
} InodeBlock;

//struct for directory entries
typedef struct DirectoryEntry {
    uint16_t open;                           //is the file open?
    uint16_t inode_index;                    //inode index
    char file_name[MAX_FILENAME_SIZE];       //NULL term ASCII filename
                                             //free if first character of filename is null
} DirectoryEntry;

//typedef for a single block bitmap, structure must be size of one block
typedef struct Bitmap {
    uint8_t bytes[SOFTWARE_DISK_BLOCK_SIZE];
} Bitmap;

//struct for main file tyoe
typedef struct FileInternals {
    uint64_t position;      //current file position
    FileMode mode;          //access mode
    Inode inode;            //inode
    DirectoryEntry dir;     //directory entry
    uint16_t dir_block;     //block # for directory entry
} FileInternals;


uint64_t allocate_bit(uint8_t *data) {
    int64_t index = -1;
    for (int64_t i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (int64_t j = 0; j <= 7; j++)
        {
            if((data[i] && (1 << (7-j))) == 0)
            {
                index = i*8*j;
                return index;
            }
        }
    }
    return index;
}

void used_bit(uint8_t *data, int64_t index) {
    for (int64_t i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (int64_t j = 0; j <= 7; j++)
        {
            if (i*8+j == index)
            {
                data[i] |= 1UL << j; //set bit in bitmap
            }
        }
    }
}

void free_bit(uint8_t *data, int64_t index) {
    for (int64_t i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (int64_t j = 0; j <= 7; j++)
        {
            if (i*8+j == index)
            {
                data[i] &= ~(1UL << j); //clear bit in bitmap
            }
        }
    }
}

File open_file(char *name, FileMode mode){
    File file;
    fserror = FS_NONE;

    if(!file_exists(name))
    {
        fserror = FS_FILE_NOT_FOUND;
        return NULL;
    }
    else
    {
        //iterate through directory entry blocks for the file
        uint16_t i;
        for(i = FIRST_DIR_ENTRY_BLOCK; i < LAST_DIR_ENTRY_BLOCK; i++)
        {
            //read the block
            char buf[SOFTWARE_DISK_BLOCK_SIZE];
            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            read_sd_block(buf, i);

            //create directory entry to copy data into
            DirectoryEntry dir;
            memcpy(&dir, &buf, sizeof(dir));
            //does the filename match?
            if(strcmp(dir.file_name, name))
            {
                //check if file is open
                if(dir.open)
                {
                    fserror = FS_FILE_OPEN;
                }
                else
                {
                    //get inode block
                    char buf1[SOFTWARE_DISK_BLOCK_SIZE];
                    bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
                    read_sd_block(buf1, FIRST_INODE_BLOCK + dir.inode_index / 4);

                    Inode n;
                    memcpy(&n, &buf1[dir.inode_index % 4], sizeof(n));
                    //make the file
                    file->position = 0;
                    file->mode = mode;
                    file->inode = n;
                    file->dir = dir;
                    file->dir_block = i;

                    //set file as opened in the software disk
                    dir.open = 1;
                    memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
                    write_sd_block(buf, i);
                    fserror = FS_NONE;
                    return file;
                }
            }
            else
            {
                fserror = FS_FILE_NOT_FOUND;
                return NULL;
            }    
        }
        return file;
    }
}

File create_file(char *name){
    File file;
    fserror = FS_NONE;
    if(strcmp(name, "\0"))
    {
        fserror = FS_ILLEGAL_FILENAME;
    }
    else if(file_exists(name))
    {
        fserror = FS_FILE_ALREADY_EXISTS;
    }
    else
    {
        //find and allocate a bit in the inode bitmap
        char buf[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        read_sd_block(buf, INODE_BITMAP_BLOCK);
        //find free bit 
        uint16_t inode_index = allocate_bit((unsigned char*) buf);
        //mark bit as used
        used_bit((unsigned char*) buf, inode_index);
        //update bitmap
        write_sd_block(buf, INODE_BITMAP_BLOCK);
        
        //create inode for file
        Inode node;
        node.file_size = 0;
        for(int i = 0; i < NUM_DIRECT_INODE_BLOCKS+1; i++)
        {

            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            read_sd_block(buf, DATA_BITMAP_BLOCK);
            //set data_index to the free bit to allocate
            uint16_t data_index = allocate_bit((unsigned char*) buf);
            //mark bit at data_index as used
            used_bit((unsigned char*) buf, data_index);
            write_sd_block(buf, INODE_BITMAP_BLOCK);
            //add it to inode blocks
            node.blocks[i] = data_index;
        }

        //read destination for inode block
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        read_sd_block(buf, FIRST_INODE_BLOCK + (inode_index / 128));

        //edit block
        //potential logic issues here
        unsigned long i = (inode_index % 128)*4;
        memcpy(&buf[i], &node, 4);
        write_sd_block(buf, FIRST_INODE_BLOCK + (inode_index / 128));

        //create directory entry and mark it as open
        DirectoryEntry dir;
        dir.open = 1;
        dir.inode_index = inode_index;
        strcpy(dir.file_name, name);

        //find block for direntry
        for (uint64_t c = FIRST_DIR_ENTRY_BLOCK; c < LAST_DIR_ENTRY_BLOCK; c++)
        {
            //read block
            char buf[SOFTWARE_DISK_BLOCK_SIZE];
            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            read_sd_block(buf, c);

            //make direntry for data
            DirectoryEntry dir;
            memcpy(&dir, &buf, sizeof(dir));

            //filename null? then break
            if(strcmp(dir.file_name, "\0"))
            {
                fserror = FS_ILLEGAL_FILENAME;
                break;
            }
        }
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
        write_sd_block(buf, i);

        file->position = 0;
        file->mode = READ_WRITE;
        file->inode = node;
        file->dir = dir;
        file->dir_block = i;

        fserror = FS_NONE;
        return file;
    }
    return file;
}

void close_file(File file){
    fserror = FS_NONE;
    if(file->dir.open == 0) 
    {
        fserror = FS_FILE_NOT_OPEN;
    }
    else
    {
        Inode inode = file->inode;
        DirectoryEntry dir = file->dir;
        uint16_t d_block = file->dir_block;

        //set file to closed
        dir.open = 0;

        //write closed entry into buffer
        char buf1[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
        memcpy(&buf1, &dir, SOFTWARE_DISK_BLOCK_SIZE);
        write_sd_block(buf1, d_block);
        fserror = FS_NONE;
    }
}

unsigned long read_file(File file, void *buf, unsigned long numbytes){
    uint32_t position = file->position;
    FileMode mode = file->mode;
    Inode inode = file->inode;
    DirectoryEntry dir = file->dir;
    uint16_t d_block = file->dir_block;
    fserror = FS_NONE;

    if(dir.open)
    {
        fserror = FS_FILE_NOT_OPEN;
        return 0;
    }
    else if(position > file_length(file))
    {
        fserror = FS_IO_ERROR;
        return 0;
    }
        else
        {
            uint32_t blocknumber = 0;
            while(numbytes > 0)
            {
                char buf1[SOFTWARE_DISK_BLOCK_SIZE];
                bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
                //read iblock
                read_sd_block(buf1, inode.blocks[blocknumber]);
                //copy into buffer
                //potential issues with sizing here
                unsigned long x = sizeof(&buf1);
                memcpy(&buf, &buf1, x);
                numbytes -= x;
                blocknumber += 1;
            }
            fserror = FS_NONE;
            return numbytes;
        }
    return numbytes;
}

unsigned long write_file(File file, void *buf, unsigned long numbytes){
    uint32_t position = file->position;
    FileMode mode = file->mode;
    Inode inode = file->inode;
    DirectoryEntry dir = file->dir;
    uint16_t d_block = file->dir_block;
    fserror = FS_NONE;

    if(dir.open)
    {
        fserror = FS_FILE_NOT_OPEN;
    }
    else if (mode == READ_ONLY)
    {
        fserror = FS_FILE_READ_ONLY;
    }
    else if (file_length(file) + numbytes > MAX_FILE_SIZE)
    {
        fserror = FS_EXCEEDS_MAX_FILE_SIZE;
    }
    else
    {
        uint32_t blocknumber = 0;
        while(numbytes > 0)
        {
            //potential sizing issues here
            char buf1[SOFTWARE_DISK_BLOCK_SIZE];
            bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
            read_sd_block(buf1, inode.blocks[blocknumber]);
            unsigned long x = sizeof(&buf1);
            memcpy(&buf, &buf1, x);
            numbytes -= x;
            blocknumber += 1;
        }
        fserror = FS_NONE;
        return numbytes;
    }  
    return numbytes; 
}

int seek_file(File file, unsigned long bytepos){
    fserror = FS_NONE;
    if(bytepos >= MAX_FILE_SIZE)
    {
        fserror = FS_EXCEEDS_MAX_FILE_SIZE;
        return 0;
    }
    else {
        //file->position = bytepos;
        fserror = FS_NONE;
        return 1;
    }
}

unsigned long file_length(File file){
    if(!file_exists(file->dir.file_name))
    {
        fserror = FS_FILE_NOT_FOUND;
        return 0;
    }
    else
    {
        fserror = FS_NONE;
        return file->inode.file_size;
    }
}

int delete_file(char *name){
    int i, j;
    fserror = FS_NONE;
    //search through directory entries for file
    for (i = FIRST_DIR_ENTRY_BLOCK; i < LAST_DIR_ENTRY_BLOCK; i++)
    {
        
        char buf1[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
        //read entry at i into buf
        read_sd_block(buf1, i);

        //create directory entry and copy data from entry i into it 
        DirectoryEntry *dir;
        memcpy(&dir, &buf1, sizeof(dir));

        //is there a matching filename?
        if(strcmp(dir->file_name, name))
        {
            //if so, clear it in the software disk
            //set name to 0
            strcpy(dir->file_name, "\0");
            memcpy(&buf1, &dir, SOFTWARE_DISK_BLOCK_SIZE);
            write_sd_block(buf1, i);
            return 1;
        }
    }
    fserror = FS_FILE_NOT_FOUND;
    return 0;
}

int file_exists(char *name){
    int i, j;
    //go through directory entries for file
    fserror = FS_NONE;
    for (i = FIRST_DIR_ENTRY_BLOCK; i < LAST_DIR_ENTRY_BLOCK; i++)
    {
        //read block
        char buf1[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
        read_sd_block(buf1, i);

        //create directory entry and copy data into it 
        DirectoryEntry *dir;

        memcpy(&dir, &buf1, sizeof(dir));

        //is there a matching filename?
        if(strcmp(dir->file_name, name))
        {
            return 1;
        }
        else
        {
            return 0;
        }
    }
    return 0;
}

void fs_print_error(void){
    switch(fserror) {
        case FS_NONE:
            printf("No error. \n");
            break;
        case FS_OUT_OF_SPACE:
            printf("FS ERROR: Out of space. \n");
            break;
        case FS_FILE_NOT_OPEN:
            printf("FS ERROR: File not open. \n");
            break;
        case FS_FILE_OPEN:
            printf("FS ERROR: File is already open. \n");
            break;
        case FS_FILE_NOT_FOUND:
            printf("FS ERROR: File not found. \n");
            break;
        case FS_FILE_READ_ONLY:
            printf("FS ERROR: File is set to read only. \n");
            break;
        case FS_FILE_ALREADY_EXISTS:
            printf("FS ERROR: File already exists. \n");
            break;
        case FS_EXCEEDS_MAX_FILE_SIZE:
            printf("FS ERROR: File exceeds max size. \n");
            break;
        case FS_ILLEGAL_FILENAME:
            printf("FS ERROR: Illegal filename. \n");
            break;
        case FS_IO_ERROR:
            printf("FS ERROR: Something really bad happened. \n");
            break;
        default:
            printf("FS ERROR: Unknown error. \n");
            break;
    }
}

int check_structure_alignment(void){
    printf("Inode size is: %lu.\n", sizeof(Inode));
    printf("Indirect block size is: %lu.\n", sizeof(IndirectBlock));
    printf("Inode block size is: %lu.\n", sizeof(InodeBlock));
    printf("Directory Entry size is: %lu.\n", sizeof(DirectoryEntry));
    printf("Bitmap size is: %lu.\n", sizeof(Bitmap));

    if(sizeof(Inode) != 32 || sizeof(IndirectBlock) != 4096 || sizeof(InodeBlock) != 4096 
    || sizeof(DirectoryEntry) != 512 || sizeof(Bitmap) != 4096) 
    {
        return 0;
    }
    else
    {
        return 1;
    }
}
