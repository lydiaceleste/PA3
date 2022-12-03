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
    uint64_t size;                              //file size in bytes
    uint16_t blocks[NUM_DIRECT_INODE_BLOCKS+1]; //direct blocks + the one indirect
} Inode;

//struct for a block on inodes
typedef struct InodeBlock {
    Inode inodes[SOFTWARE_DISK_BLOCK_SIZE / sizeof(Inode)]; //each inode block holds 128 inodes
} InodeBlock;

//struct for directory entries
typedef struct DirectoryEntry {
    uint16_t file_open;                 //is the file open?
    uint16_t inode_index;               //inode index
    char name[MAX_FILENAME_SIZE];       //NULL term ASCII filename
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
    uint16_t d_block;       //block # for directory entry
} FileInternals;



uint64_t allocate_bit(uint8_t *data) {
    int64_t index = -1;
    int64_t i, j;
    for (i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (j = 0; j <= 7; j++)
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
    int64_t i, j;
    for (i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (j = 0; j <= 7; j++)
        {
            if (i*8+j == index)
            {
                data[i] |= 1UL << j; //set this bit in bitmap
            }
        }
    }
}

void free_bit(uint8_t *data, int64_t index) {
    int64_t i, j;
    for (i = 0; i < SOFTWARE_DISK_BLOCK_SIZE; i++)
    {
        for (j = 0; j <= 7; j++)
        {
            if (i*8+j == index)
            {
                data[i] &= ~(1UL << j); //clear this bit in bitmap
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
            int ret;
            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            ret = read_sd_block(buf, i);

            //create directory entry to copy data into
            DirectoryEntry dir;
            memcpy(&dir, &buf, sizeof(dir));
            //does the filename match?
            if(strcmp(dir.name, name))
            {
                //check if file is open
                if(dir.file_open)
                {
                    fserror = FS_FILE_OPEN;
                }
                else
                {
                    //get inode block
                    char buf1[SOFTWARE_DISK_BLOCK_SIZE];
                    bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
                    ret = read_sd_block(buf1, FIRST_INODE_BLOCK + dir.inode_index / 16);

                    Inode n;
                    memcpy(&n, &buf1[dir.inode_index%4], sizeof(n));
                    //make the file
                    file->position = 0;
                    file->mode = mode;
                    file->inode = n;
                    file->dir = dir;
                    file->d_block = i;

                    //set file as opened in the software disk
                    dir.file_open = 1;
                    memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
                    ret = write_sd_block(buf, i);
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
        //emptying 4096 bytes in buf
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        //read inode bitmap into buffer
        read_sd_block(buf, INODE_BITMAP_BLOCK);
        //find free bit for file in bitmap
        uint16_t inode_index = allocate_bit((unsigned char*) buf);
        //mark bit as used
        used_bit((unsigned char*) buf, inode_index);
        //update bitmap
        write_sd_block(buf, INODE_BITMAP_BLOCK);

        //create inode for file
        Inode n;
        n.size = 0;
        for(int i = 0; i < NUM_DIRECT_INODE_BLOCKS+1; i++)
        {

            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            // reads a block of data into 'buf' from the data bitmap block
            read_sd_block(buf, DATA_BITMAP_BLOCK);
            //set data_index to the free bit to allocate
            uint16_t data_index = allocate_bit((unsigned char*) buf);
            //mark bit at data_index as used
            used_bit((unsigned char*) buf, data_index);
            // writes a block of data from 'buf' at the location of the inode bitmap
            write_sd_block(buf, INODE_BITMAP_BLOCK);
            //add it to inode blocks
            n.blocks[i] = data_index;
        }

        //read destination for inode block
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        //index is where the inode is in the bitmap
        //reads data from FIRST_INODE_BLOCK + (inode_index / 128) into buf
        read_sd_block(buf, FIRST_INODE_BLOCK + (inode_index / 128));

        //edit block
        unsigned long i = (inode_index % 128)*4;
        memcpy(&buf[i], &n, 4);
        write_sd_block(buf, FIRST_INODE_BLOCK + (inode_index / 128));

        //create directory entry and mark it as open
        DirectoryEntry dir;
        dir.file_open = 1;
        dir.inode_index = inode_index;
        strcpy(dir.name, name);

        //find block for said directory entry
        for (uint64_t c = FIRST_DIR_ENTRY_BLOCK; c < LAST_DIR_ENTRY_BLOCK; c++)
        {
            //read that block
            char buf[SOFTWARE_DISK_BLOCK_SIZE];
            int ret;
            bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
            read_sd_block(buf, c);

            //create directory entry and copy data into it
            DirectoryEntry dir;
            memcpy(&dir, &buf, sizeof(dir));

            //filename null? then break
            if(strcmp(dir.name, "/0"))
            {
                break;
            }
        }
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
        write_sd_block(buf, i);

        //create file
        file->position = 0;
        file->mode = READ_WRITE;
        file->inode = n;
        file->dir = dir;
        file->d_block = i;

        fserror = FS_NONE;
        return file;
    }
    return file;
}

void close_file(File file){
    fserror = FS_NONE;
    if(file->dir.file_open == 0) 
    {
        fserror = FS_FILE_NOT_OPEN;
    }
    else
    {
        Inode inode = file->inode;
        DirectoryEntry dir = file->dir;
        uint16_t d_block = file->d_block;

        //set file to closed
        dir.file_open = 0;

        //write closed entry into buffer
        char buf[SOFTWARE_DISK_BLOCK_SIZE];
        int ret;
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
        ret = write_sd_block(buf, d_block);
        fserror = FS_NONE;
    }
}

unsigned long read_file(File file, void *buf, unsigned long numbytes){
    
    uint32_t position = file->position;
    FileMode mode = file->mode;
    Inode inode = file->inode;
    DirectoryEntry dir = file->dir;
    uint16_t d_block = file->d_block;
    fserror = FS_NONE;
    if(dir.file_open)
    {
        fserror = FS_FILE_NOT_OPEN;
    }
    else
    {
        if(!file_exists(dir.name))
        {
            fserror = FS_FILE_NOT_FOUND;
        }
        else
        {
            uint32_t blocknumber = 0;
            while(numbytes > 0)
            {
                char buf[SOFTWARE_DISK_BLOCK_SIZE];
                char buf1[SOFTWARE_DISK_BLOCK_SIZE];
                bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
                //read iblock
                read_sd_block(buf1, inode.blocks[blocknumber]);
                //copy into buffer
                memcpy(&buf, &buf1, sizeof(buf1));
                numbytes -= sizeof(&buf1);
                blocknumber += 1;
            }
            fserror = FS_NONE;
            return numbytes;
        }
    }
    return numbytes;
}

unsigned long write_file(File file, void *buf, unsigned long numbytes){
    uint32_t position = file->position;
    FileMode mode = file->mode;
    Inode inode = file->inode;
    DirectoryEntry dir = file->dir;
    uint16_t d_block = file->d_block;
    fserror = FS_NONE;
    if(dir.file_open)
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
        if(!file_exists(dir.name))
        {
            fserror = FS_FILE_NOT_FOUND;
        }
        else
        {
            uint32_t blocknumber = 0;
            while(numbytes > 0)
            {
                char buf1[SOFTWARE_DISK_BLOCK_SIZE];
                bzero(buf1, SOFTWARE_DISK_BLOCK_SIZE);
                read_sd_block(buf1, inode.blocks[blocknumber]);
                memcpy(&buf, &buf1, sizeof(buf));
                numbytes -= sizeof(&buf1);
                blocknumber += 1;
            }
            return numbytes;
        }
        fserror = FS_NONE;
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
    if(!file_exists(file->dir.name))
    {
        fserror = FS_FILE_NOT_FOUND;
        return 0;
    }
    else
    {
        fserror = FS_NONE;
        return file->inode.size;
    }
}

int delete_file(char *name){
    int i, j;
    fserror = FS_NONE;
    //search through directory entries for file
    for (i = FIRST_DIR_ENTRY_BLOCK; i < LAST_DIR_ENTRY_BLOCK; i++)
    {
        
        char buf[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        //read entry at i into buf
        read_sd_block(buf, i);

        //create directory entry and copy data from entry i into it 
        DirectoryEntry *dir;
        memcpy(&dir, &buf, sizeof(dir));

        //is there a matching filename?
        if(strcmp(dir->name, name))
        {
            //if so, clear it in the software disk
            //set name to 0
            strcpy(dir->name, "\0");
            memcpy(&buf, &dir, SOFTWARE_DISK_BLOCK_SIZE);
            write_sd_block(buf, i);
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
        char buf[SOFTWARE_DISK_BLOCK_SIZE];
        bzero(buf, SOFTWARE_DISK_BLOCK_SIZE);
        read_sd_block(buf, i);

        //create directory entry and copy data into it 
        DirectoryEntry *dir;

        memcpy(&dir, &buf, sizeof(dir));

        //is there a matching filename?
        if(strcmp(dir->name, name))
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
            printf("FS ERROR: Error doing IO. \n");
            break;
        default:
            printf("FS ERROR: Unknown error. \n");
            break;
    }
}

/**
int check_structure_alignment(void){
    return 1;
}
**/