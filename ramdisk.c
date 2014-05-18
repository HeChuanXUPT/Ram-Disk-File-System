#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <asm/string.h>
#include <asm/unistd.h>

#include "ramdisk.h"        
#include "ramdisk_ioctl.h"



static char* ramdisk;                                     //the starting pointer
static superblock* sb;                                    //superblock
static inode* inodeArray;                                 //inode
static fileDescriptorNode* fileDescriptorProcessList;     //file descriptor
static struct file_operations ramdiskOperations;          //file operation
static struct proc_dir_entry *proc_entry;                 //proc entry
static struct proc_dir_entry *proc_backup;               
static char procfs_buffer[RAMDISK_SIZE];

//management of bitmap section
//
int getpid(void) 
{
    return current->pid;
}

//determine the position of bit
unsigned int bitPosition(unsigned int index) 
{
    return (0x01 << index);
}

//get the value of bit
unsigned int getBit(unsigned int* value, int bitPosition) 
{
    return (*value & bitPosition);
}

//set the value of bit
void setBit(unsigned int* value, int bitPosition) 
{
    *value |= bitPosition;
}

//cleat the bit
void clearBit(unsigned int* value, int bitPosition) 
{
    *value &= ~(bitPosition);
}


int getMin(int a, int b) 
{
        if (a <= b) 
        {
            return a;
        }
        else           
        {
            return b;
        }
}


//bitmap
void initBlockBitmap(void) 
{
    char* blockBitmapIter;
    int i;
    blockBitmapIter = sb->blockBitmapStart;
    for (i = 0; i < BLOCK_BITMAP_SIZE; i++) 
    {
        *blockBitmapIter = FREE;
        blockBitmapIter++;
    }
}

//management of blocks
char* getFreeBlock(void) 
{
    int bitNumber;
    int byteNumber;
    int blockNumber;
    char* blockAddress;

    for (byteNumber = 0; byteNumber < BLOCK_BITMAP_SIZE; byteNumber++) 
    {
        for (bitNumber = 0; bitNumber < 8; bitNumber++) 
        {
            if (getBit((unsigned int *)(byteNumber + sb->blockBitmapStart), bitPosition(bitNumber)) == FREE) 
            {
                sb->freeBlocks--;
                setBit((unsigned int *)(sb->blockBitmapStart + byteNumber), bitPosition(bitNumber));
                blockNumber = (byteNumber * 8) + bitNumber;
                blockAddress = sb->freeBlockStart + (RD_BLOCK_SIZE * blockNumber);  
                memset(blockAddress, 0, RD_BLOCK_SIZE);
                return blockAddress;         
            }             
        }
    }

    return NULL;
}


void printBlockBitmap(void) {
    int byteNumber;
    int bitNumber;
    int bitValue;


    for (byteNumber = 0; byteNumber < BLOCK_BITMAP_SIZE; byteNumber++) 
    {
        for (bitNumber = 0; bitNumber < 8; bitNumber++) 
        {
            bitValue = getBit((unsigned int *)(byteNumber + sb->blockBitmapStart), bitPosition(bitNumber));
        }
    }
}

//management of inodes      
void initInodeArray(void) {
    int i, j;
    for (i = 0; i < INODE_COUNT; i++) 
    {
        inodeArray[i].inodeNumber = i;
        inodeArray[i].status = FREE;
        strcpy(inodeArray[i].type, "nul");
        inodeArray[i].size = 0;
        for (j = 0; j < INODE_BLOCK_POINTERS; j++) 
        {     
            inodeArray[i].location[j] = NULL;
        }
        inodeArray[i].locationCount = 0;
    }
}


//initializes the ramdisk, allocates 2MB memory and sets up all the starting pointers of all section
int initRamdisk(void) 
{
    //all starting pointers
    char* superblockStart;
    char* inodeArrayStart;
    char* blockBitmapStart;
    char* freeBlockStart;
    int freeBlocks, freeInodes;
    
    //alloctes 2MB memory to the ramdisk
    ramdisk = (void*) vmalloc(RAMDISK_SIZE);
    if (!ramdisk) 
    {
        printk("There is no memory for ramdisk.\n");
        return -1;
    }

    superblockStart = ramdisk;
    inodeArrayStart = superblockStart + SUPERBLOCK_SIZE; 
    blockBitmapStart = inodeArrayStart + INODE_SIZE;
    freeBlockStart = blockBitmapStart + BLOCK_BITMAP_SIZE;
    
    //calculates the number of free blocks and inodes
    freeBlocks = (RAMDISK_SIZE - SUPERBLOCK_SIZE - INODE_SIZE - BLOCK_BITMAP_SIZE) / RD_BLOCK_SIZE;
    freeInodes = INODE_SIZE / INODE_STRUCTURE_SIZE;
    

    sb = (superblock*)superblockStart;
    sb->freeBlocks = freeBlocks;
    sb->freeInodes = freeInodes - 1;// the root
    sb->blockBitmapStart = blockBitmapStart;
    sb->freeBlockStart = freeBlockStart;

    inodeArray = (inode*)inodeArrayStart;
    initBlockBitmap();
    initInodeArray();

    inodeArray[0].inodeNumber = 0;
    inodeArray[0].status = ALLOCATED;
    strcpy(inodeArray[0].type, "dir");
    inodeArray[0].size = 0;
    inodeArray[0].location[0] = getFreeBlock();
    inodeArray[0].locationCount++;
    return 1;
}

//free the ramdisk
void destroyRamdisk(void) 
{
    if (ramdisk) {
        vfree(ramdisk);
    }
    if (fileDescriptorProcessList) {
        vfree(fileDescriptorProcessList);
    }
    ramdisk = NULL;
    sb = NULL;
    inodeArray = NULL;
}

//sets up and initializes the file descriptor
void initFileDescriptorTable(void) 
{
    fileDescriptorNode* dumramHead;
    dumramHead = (fileDescriptorNode*) vmalloc(sizeof(fileDescriptorNode));
    dumramHead->pid = 0;
    dumramHead->next = NULL;
    fileDescriptorProcessList = dumramHead;
}

//
fileDescriptorNode* findFileDescriptor(fileDescriptorNode* trav, int pid) 
{
    while (trav) 
    {
        if (trav->pid == pid) 
        {
            return trav;
        }
        trav = trav->next;
    }
    return NULL;
}

//gets the index of file descriptor
int getFileDescriptorIndex(fileDescriptorNode* pointer) 
{
    int i;
    for (i = 0; i < MAX_FILES_OPEN; i++) 
    {
        if (pointer->fileDescriptorTable[i].filePosition == -1)
        {
            return i;
        }
    }
    return -1;
}

//create the file descriptor
int createFileDescriptor(int pid, int inodeNumber) 
{
    fileDescriptorNode* check;
    int fd, i;
    check = findFileDescriptor(fileDescriptorProcessList, getpid());

    if (check != NULL) 
    {
        if (check->pid == pid) 
        {
            fd = getFileDescriptorIndex(check);
            check->fileDescriptorTable[fd].inodePointer = &inodeArray[inodeNumber];
            check->fileDescriptorTable[fd].filePosition = 0;

            return fd;
        }
    }
    else 
    {
        fileDescriptorNode* newEntry = (fileDescriptorNode*) vmalloc(sizeof(fileDescriptorNode));

        for (i = 0; i < MAX_FILES_OPEN; i++) 
        {
            newEntry->fileDescriptorTable[i].filePosition = -1;
            newEntry->fileDescriptorTable[i].inodePointer = NULL;
        }

        newEntry->pid = pid;
        newEntry->next = fileDescriptorProcessList;
        fileDescriptorProcessList = newEntry;

        fd = getFileDescriptorIndex(newEntry);

        newEntry->fileDescriptorTable[fd].inodePointer = &inodeArray[inodeNumber];
        newEntry->fileDescriptorTable[fd].filePosition = 0;
        return fd;
    }
    return -1;
}


void parse(char* pathname, char** parents, char** fileName) 
{
    char* copyPathname;
    char* lastSlash;
    int indexOfLastSlash;
    char* temp;
    int length;

    length = strlen(pathname);
    copyPathname = (char*) vmalloc(sizeof(char) * (length + 1));
    strcpy(copyPathname, pathname);
    lastSlash = strrchr(copyPathname, '/');
    indexOfLastSlash = lastSlash - copyPathname;

    *parents = (char*) vmalloc(sizeof(char) * indexOfLastSlash + 2);
    memcpy(*parents, copyPathname, indexOfLastSlash + 1);
        temp = *parents;
        temp += (indexOfLastSlash + 1);
        *temp = '\0';

    *fileName = (char*) vmalloc(sizeof(char) * (length - indexOfLastSlash));
    strcpy(*fileName, lastSlash + 1);
    
}

void setBitmap(char* blockPointer) {
    int blockNumber;
    int bitmapByteIndex;
    int bitmapBitIndex;
    char* positionInBitmap;

    blockNumber = ((blockPointer - sb->freeBlockStart) / RD_BLOCK_SIZE);

    bitmapByteIndex = blockNumber / 8;
    bitmapBitIndex  = blockNumber % 8;

    positionInBitmap = sb->blockBitmapStart + bitmapByteIndex;
    sb->freeBlocks++;

    clearBit((unsigned int*)positionInBitmap, bitPosition(bitmapBitIndex));
}

char* allocateBlock(inode* node) 
{
    int locationCount;
    int iter, iter1,iter2;
    singleIndirectLevel* level;
    doubleIndirectLevel* doubleLevel;

    level = NULL;
    doubleLevel = NULL;
    locationCount = node->locationCount;

    if (locationCount < 8) 
    {
        node->location[locationCount] = getFreeBlock();
        node->locationCount++;
        return node->location[locationCount];
    }

    else if (locationCount == 8) {
        node->location[8] = getFreeBlock(); // stores 64 ptrs
        level = (singleIndirectLevel*) node->location[8];
        level->pointers[0] = getFreeBlock();
        node->locationCount++;
        return level->pointers[0];
    }
    
    else if (locationCount == 9) 
    {
        level = (singleIndirectLevel*) node->location[8];
        
        for (iter = 0; iter < 64; iter++) 
        {
            if (level->pointers[iter] == NULL) 
            {
                level->pointers[iter] = getFreeBlock();
                return level->pointers[iter];
            }
        }

        node->location[9] = getFreeBlock();
        doubleLevel = (doubleIndirectLevel*) node->location[9];
        doubleLevel->pointers[0] = (singleIndirectLevel*) getFreeBlock();
        doubleLevel->pointers[0]->pointers[0] = getFreeBlock();
        node->locationCount++;
        return doubleLevel->pointers[0]->pointers[0];
    }

    else if (locationCount == 10) 
    {
        doubleLevel = (doubleIndirectLevel*) node->location[9];
        for (iter1 = 0; iter1 < 64; iter1++) 
        {
            if (doubleLevel->pointers[iter1] == NULL) 
            {
                doubleLevel->pointers[iter1] = (singleIndirectLevel*) getFreeBlock();
                doubleLevel->pointers[iter1]->pointers[0] = getFreeBlock();
                return doubleLevel->pointers[iter1]->pointers[0];
            }
            level = doubleLevel->pointers[iter1];
            for (iter2 = 0; iter2 < 64; iter2++) 
            {
                if (level->pointers[iter2] == NULL) 
                {
                    level->pointers[iter2] = getFreeBlock();
                    return level->pointers[iter2];
                }
            }
        }
    }

    return NULL;
}

int existsInBlock(char* blockAddress, char* fileName, char* type) 
{
    dirEntry* dirTraverser;
    char* dirEntryFileName;
    int dirEntryInodeNumber;
    char* dirEntryType;
    int i;
    char* dirEntryIter;
    int entryCount;

    dirEntryIter = blockAddress;
    entryCount = RD_BLOCK_SIZE / DIR_ENTRY_STRUCTURE_SIZE;
    for (i = 0; i < entryCount; i++) {
        dirTraverser = (dirEntry*) dirEntryIter;
        dirEntryFileName = dirTraverser->fileName;
        dirEntryInodeNumber = dirTraverser->inodeNumber;
        dirEntryType = inodeArray[dirEntryInodeNumber].type;
    
        if (strcmp(dirEntryFileName, "/") != 0 && dirEntryInodeNumber == 0) 
        {
            return -1;
        }

        if (strcmp(dirEntryFileName, fileName) == 0) 
        {
            if ((strcmp("ign", type) == 0) || (strcmp(dirEntryType, type) == 0))
            {        
                return dirEntryInodeNumber;
            }
        }        
        
        dirEntryIter += DIR_ENTRY_STRUCTURE_SIZE;
    }

    return -1;
}

int getLastEntry(char* blockAddress, char** lastEntry) 
{
    dirEntry* dirTraverser;
    char* dirEntryIterPrev;
    char* dirEntryIter;
    int i;
    int entryCount;
    char* dirEntryFileName;
    int dirEntryInodeNumber;

    entryCount = RD_BLOCK_SIZE / DIR_ENTRY_STRUCTURE_SIZE;
    dirEntryIter = dirEntryIterPrev = blockAddress;

    for (i = 0; i < entryCount; i++) 
    {
        dirTraverser = (dirEntry*) dirEntryIter;
        dirEntryFileName = dirTraverser->fileName;
        dirEntryInodeNumber = dirTraverser->inodeNumber;

        if (strcmp(dirEntryFileName, "/") != 0 && dirEntryInodeNumber == 0) 
        {
            if (dirEntryIter == dirEntryIterPrev) 
            {
                *lastEntry = NULL;
                return -1;
            }
            *lastEntry = dirEntryIterPrev;
            return 1;
        }
        
        dirEntryIterPrev = dirEntryIter;
        dirEntryIter += DIR_ENTRY_STRUCTURE_SIZE;
    }
        
    *lastEntry = dirEntryIterPrev;

    return 1;
}


char* scanBlockForFreeSlot(char* blockAddress) {
    dirEntry* dirTraverser;
    char* dirEntryIter = blockAddress;
    int i;
    char* dirEntryFileName;
    int dirEntryInodeNumber;
    int entryCount;
    entryCount = RD_BLOCK_SIZE / DIR_ENTRY_STRUCTURE_SIZE;
    for (i = 0; i < entryCount; i++) 
    {
        dirTraverser = (dirEntry*) dirEntryIter;
        dirEntryFileName = dirTraverser->fileName;
        dirEntryInodeNumber = dirTraverser->inodeNumber;
        if (strcmp(dirEntryFileName, "/") != 0 && dirEntryInodeNumber == 0) {
            return dirEntryIter;
        }
        dirEntryIter += DIR_ENTRY_STRUCTURE_SIZE;
    }
    return NULL;
}

char* findLastEntry(int inodeNumber) {
    int locationCount;
    char* blockAddress;
    singleIndirectLevel* indirectBlock;
    doubleIndirectLevel* doubleIndirectBlock;
    char* dirEntryIter;
    int i, j, count;
    char* lastEntry;
    int ret;

    locationCount = inodeArray[inodeNumber].locationCount;
    
   
    if (locationCount == 10) 
    {
        doubleIndirectBlock = (doubleIndirectLevel*) inodeArray[inodeNumber].location[9];
        for (i = 63; i >= 0; i--) 
        {
            indirectBlock = doubleIndirectBlock->pointers[i];

            if (!indirectBlock) 
            {
                continue;
            }

            for (j = 63; j >= 0; j--) 
            {
                dirEntryIter = indirectBlock->pointers[j];

                if (!dirEntryIter) {
                    continue;
                }
                
                ret = getLastEntry(dirEntryIter, &lastEntry);
                if (ret == 1) 
                {
                    return lastEntry;
                }

                else if (ret == -1) 
                {
                    setBitmap(dirEntryIter);
                    indirectBlock->pointers[j] = NULL;
                    if (j == 0) 
                    {
                        setBitmap((char*)indirectBlock);
                        doubleIndirectBlock->pointers[i] = NULL;
                    }
                }
            }
        }
        setBitmap(inodeArray[inodeNumber].location[9]);
        inodeArray[inodeNumber].location[9] = NULL;
        inodeArray[inodeNumber].locationCount--;
    }

    locationCount = inodeArray[inodeNumber].locationCount;
    if (locationCount > 8) 
    {
        // Get the base address of the block holding indirect pointers
        indirectBlock = (singleIndirectLevel*) inodeArray[inodeNumber].location[8];

        // Iterate all 64 block pointers
        for (i = 63; i >= 0; i--) {
            // Get block pointed to by indirect pointer
            dirEntryIter = indirectBlock->pointers[i];

            // If this single indirect block has no more pointers to blocks...
            if (!dirEntryIter) 
            {
                continue;
            }

            // Check if this file exists in the block
            ret = getLastEntry(dirEntryIter, &lastEntry);
            // If the files inode number is not -1, then return this inode
            if (ret == 1) 
            {
                    return lastEntry;
            }
            else if (ret == -1) 
            {
                setBitmap(dirEntryIter);
                indirectBlock->pointers[i] = NULL;
                if (i == 0) 
                {
                    setBitmap(inodeArray[inodeNumber].location[8]);
                    inodeArray[inodeNumber].location[8] = NULL;
                    inodeArray[inodeNumber].locationCount--;
                }
            }
        }
    }

    locationCount = inodeArray[inodeNumber].locationCount;
    count = locationCount - 1;
    while (count >= 0) 
    {
        blockAddress = inodeArray[inodeNumber].location[count];
                     
        ret = getLastEntry(blockAddress, &lastEntry);
        if (ret == 1) 
        {
            return lastEntry;
        }
        else if (ret == -1) 
        {
            setBitmap(blockAddress);
            inodeArray[inodeNumber].locationCount--;
        }
            
        count--;
    }                
    
    return NULL;
}

int deleteFromBlock(char* blockAddress, char* fileName, char* type, int parentInodeNumber) {
    dirEntry* dirTraverser;
    char* dirEntryIter = blockAddress;
    int i;
    int entryCount;
    char* dirEntryFileName;
    int dirEntryInodeNumber;
    char* dirEntryType;
    char* lastEntry;

    entryCount = RD_BLOCK_SIZE / DIR_ENTRY_STRUCTURE_SIZE;

    for (i = 0; i < entryCount; i++) 
    {
        dirTraverser = (dirEntry*) dirEntryIter;
        dirEntryFileName = dirTraverser->fileName;
        dirEntryInodeNumber = dirTraverser->inodeNumber;
        dirEntryType = inodeArray[dirEntryInodeNumber].type;

        if (strcmp(dirEntryFileName, "/") != 0 && dirEntryInodeNumber == 0) 
        {
            return -1;
        }

        if (strcmp(dirEntryFileName, fileName) == 0) 
        {
            if ((strcmp("ign", type) == 0) || (strcmp(dirEntryType, type) == 0)) 
            {
                lastEntry = findLastEntry(parentInodeNumber);

                memcpy(dirEntryIter, lastEntry, DIR_ENTRY_STRUCTURE_SIZE);
                memset(lastEntry, 0, DIR_ENTRY_STRUCTURE_SIZE);

                return dirEntryInodeNumber;
            }
        }
        dirEntryIter += DIR_ENTRY_STRUCTURE_SIZE;
    }
    return -1;
}

int isDirEntry(int inodeNumber, char* fileName, char* type) 
{
    int locationCount;
    int directCount;
    int dirEntryInodeNumber;
    char* blockAddress;
    singleIndirectLevel* indirectBlock;
    doubleIndirectLevel* doubleIndirectBlock;
    char* dirEntryIter;
    int i, pointerCount, count;
    
    directCount = count = 0;
    pointerCount = 64;
    locationCount = inodeArray[inodeNumber].locationCount;
    if (locationCount > 8) 
    {
        directCount = 8;
    }
    else 
    {
        directCount = locationCount;
    }
 
    while (count < directCount) 
    {
        blockAddress = inodeArray[inodeNumber].location[count];
        
        dirEntryInodeNumber = existsInBlock(blockAddress, fileName, type);

        if (dirEntryInodeNumber != -1) 
        {
            return dirEntryInodeNumber;
        }

        count++;                
    }

    if (locationCount > 8) 
    {
        indirectBlock = (singleIndirectLevel*) inodeArray[inodeNumber].location[8];

        for (i = 0; i < pointerCount; i++) 
        {
            dirEntryIter = indirectBlock->pointers[i];
   
            if (!dirEntryIter) {
                break;
            }

            dirEntryInodeNumber = existsInBlock(dirEntryIter, fileName, type);
            if (dirEntryInodeNumber != -1)
            {
                return dirEntryInodeNumber;
            }
        }
    }

    if (locationCount == 10) 
    {
        doubleIndirectBlock = (doubleIndirectLevel*) inodeArray[inodeNumber].location[9];
        
        for (i = 0; i < pointerCount; i++) 
        {
            indirectBlock = doubleIndirectBlock->pointers[i];
    
            if (!indirectBlock) 
            {
                break;
            }

            for (i = 0; i < pointerCount; i++) 
            {
                dirEntryIter = indirectBlock->pointers[i];

                if (!dirEntryIter) 
                {
                    break;
                }

                dirEntryInodeNumber = existsInBlock(dirEntryIter, fileName, type);

                if (dirEntryInodeNumber != -1) 
                {
                    return dirEntryInodeNumber;
                }
            }
        }    
    }
    return -1;
}

int unlinkHelper(int inodeNumber, char* fileName, char* type) 
{
    int locationCount;
    int directCount;
    int dirEntryInodeNumber;
    char* blockAddress;
    singleIndirectLevel* indirectBlock;
    doubleIndirectLevel* doubleIndirectBlock;
    char* dirEntryIter;
    int i, pointerCount, count;

    directCount = count = 0;
    pointerCount = 64;
    locationCount = inodeArray[inodeNumber].locationCount;

    if (locationCount > 8)
    {
        directCount = 8;
    }
    else 
    {
        directCount = locationCount;
    }

    while (count < directCount) 
    {
        blockAddress = inodeArray[inodeNumber].location[count];

        dirEntryInodeNumber = deleteFromBlock(blockAddress, fileName, type, inodeNumber);

        if (dirEntryInodeNumber != -1) {
            return dirEntryInodeNumber;
        }

        count++;
    }

    if (locationCount > 8) 
    {
        indirectBlock = (singleIndirectLevel*) inodeArray[inodeNumber].location[8];

        for (i = 0; i < pointerCount; i++) 
        {
            dirEntryIter = indirectBlock->pointers[i];

            if (!dirEntryIter)
            {
                break;
            }

            dirEntryInodeNumber = deleteFromBlock(dirEntryIter, fileName, type, inodeNumber);
            if (dirEntryInodeNumber != -1) 
            {
                return dirEntryInodeNumber;
            }
        }
    }

    if (locationCount == 10) 
    {
        doubleIndirectBlock = (doubleIndirectLevel*) inodeArray[inodeNumber].location[9];

        for (i = 0; i < pointerCount; i++) 
        {
            indirectBlock = doubleIndirectBlock->pointers[i];

            if (!indirectBlock) 
            {
                break;
            }

            for (i = 0; i < pointerCount; i++) 
            {
                dirEntryIter = indirectBlock->pointers[i];

                if (!dirEntryIter) 
                {
                    break;
                }
                dirEntryInodeNumber = deleteFromBlock(dirEntryIter, fileName, type, inodeNumber);

                if (dirEntryInodeNumber != -1) 
                {
                    return dirEntryInodeNumber;
                }
            }
        }
    }
    return -1;
}

int getDirInodeNumber(char* pathname) {
    char* dirName;
    int inodeNumber;
    int dirInodeNum;

    dirName = strsep(&pathname, "/");
    dirName = strsep(&pathname, "/");

    inodeNumber = 0;

    while (dirName && strlen(dirName) > 0) 
    {
        dirInodeNum = isDirEntry(inodeNumber, dirName, "dir");
        if (dirInodeNum > -1) 
        {
            inodeNumber = dirInodeNum;
        }
        else 
        {
            return -1;
        }

        dirName = strsep(&pathname, "/");
    }
    return inodeNumber;
}       


int validateFile(char* pathname, char* type) {
    char* fileName;
        char* parents; 
    int parentInodeNum;
    int isDir;

    fileName = parents = NULL;
    parse(pathname, &parents, &fileName);

    parentInodeNum = getDirInodeNumber(parents);
    vfree(parents);
    parents = NULL;

    if (parentInodeNum == -1) 
    {
        vfree(fileName);
        fileName = NULL;
        return -1;
    }


    isDir = isDirEntry(parentInodeNum, fileName, type);
    vfree(fileName);
    fileName = NULL;

    if (isDir == -1) 
    {
        return parentInodeNum;
    }
    else
    {
        return -1;
    }
}

dirEntry* getFreeDirEntry(int inodeNumber) {
    int locationCount;
    char* freeSlot;
    char* blockAddress;
    singleIndirectLevel* indirectBlock;
    doubleIndirectLevel* doubleIndirectBlock;
    char* dirEntryIter;
    int i, pointerCount, count;

    pointerCount = 64;
    locationCount = inodeArray[inodeNumber].locationCount;
    count = 0;

    if (locationCount <= 8) 
    {
            blockAddress = inodeArray[inodeNumber].location[locationCount - 1];

            // See if this block has a vfree slot
            freeSlot = scanBlockForFreeSlot(blockAddress);

            if (freeSlot) {
                return ((dirEntry*) freeSlot);
            }
    }

    else if (locationCount == 9) 
    {
        indirectBlock = (singleIndirectLevel*) inodeArray[inodeNumber].location[8];

        for (i = 0; i < pointerCount; i++) {
            dirEntryIter = indirectBlock->pointers[i];

            if (!dirEntryIter) 
            {
                break;
            }
            freeSlot = scanBlockForFreeSlot(dirEntryIter);
            if (freeSlot) 
            {
                return ((dirEntry*) freeSlot);
            }
        }
    }

    else if (locationCount == 10) 
    {
        doubleIndirectBlock = (doubleIndirectLevel*) inodeArray[inodeNumber].location[9];

        for (i = 0; i < pointerCount; i++) 
        {
            indirectBlock = doubleIndirectBlock->pointers[i];

            if (!indirectBlock) 
            {
                break;
            }

            for (i = 0; i < pointerCount; i++) 
            {
                dirEntryIter = indirectBlock->pointers[i];

                if (!dirEntryIter) 
                {
                    break;
                }

                freeSlot = scanBlockForFreeSlot(dirEntryIter);

                if (freeSlot) 
                {
                    return ((dirEntry*) freeSlot);
                }
            }
        }
    }
    
    inode* inodePointer = &inodeArray[inodeNumber];
    dirEntry* freeEntry = (dirEntry*) allocateBlock(inodePointer);
    return freeEntry;
}


int mapFilepositionToMemAddr(inode* pointer, int filePosition, char** filePositionAddress) 
{
    int locationNumber;
    int shiftWithinBlock;
    char* filePositionTemp;
    char* blockPtr;
    int indirectShift;

    if (filePosition < DIRECT_LIMIT) 
    {
        filePositionTemp = NULL;
        locationNumber = filePosition / RD_BLOCK_SIZE;

        filePositionTemp = pointer->location[locationNumber];

        shiftWithinBlock = filePosition % RD_BLOCK_SIZE;
        filePositionTemp += shiftWithinBlock;
        *filePositionAddress = filePositionTemp;
    }

    else if (filePosition < SINGLE_INDIRECT_LIMIT) 
    {
        singleIndirectLevel* firstLevel = (singleIndirectLevel*) pointer->location[8];
        filePosition -= DIRECT_SIZE;

        indirectShift = filePosition / RD_BLOCK_SIZE;
        blockPtr = firstLevel->pointers[indirectShift];

        shiftWithinBlock = filePosition % RD_BLOCK_SIZE;
        blockPtr += shiftWithinBlock;
        *filePositionAddress = blockPtr;
    }

    else if (filePosition < DOUBLE_INDIRECT_LIMIT) {
        blockPtr = NULL;
        doubleIndirectLevel* doubleLevel = (doubleIndirectLevel*) pointer->location[9];
        singleIndirectLevel* singleLevel;

        filePosition -= SINGLE_INDIRECT_SIZE;
        filePosition -= DIRECT_SIZE;

        indirectShift = filePosition / SINGLE_INDIRECT_SIZE;

        singleLevel = doubleLevel->pointers[indirectShift];

        filePosition -= (indirectShift * SINGLE_INDIRECT_SIZE);

        indirectShift = filePosition / RD_BLOCK_SIZE;
        blockPtr = singleLevel->pointers[indirectShift];

        shiftWithinBlock = filePosition % RD_BLOCK_SIZE;
        blockPtr += shiftWithinBlock;
        *filePositionAddress = blockPtr;
    }
    else 
    {
        return -1;
    }

    return RD_BLOCK_SIZE - shiftWithinBlock;
}


// Returns the file descriptor index of pathname given a pointer to the
// file descriptor node containing a file descriptor table.
int findFileDescriptorIndexByPathname(fileDescriptorNode* pointer, char* pathname) {
    int i;
    int parentInodeNumber;
    int fileInodeNumber;
    char* fileName;
    char* parents;
    char* inodePointer;
    inode inodePtr;
    
    fileName = parents = inodePointer = NULL;

    parse(pathname, &parents, &fileName);

    parentInodeNumber = getDirInodeNumber(parents);
    fileInodeNumber = isDirEntry(parentInodeNumber, fileName, "reg");
    inodePtr = inodeArray[fileInodeNumber];
    inodePointer = (char*) &inodePtr;
    for (i = 0; i < MAX_FILES_OPEN; i++) 
    {
        if ((char*) pointer->fileDescriptorTable[i].inodePointer == inodePointer)
        {
                return i;
        }
    }
    return -1;
}


int isFileInFDProcessList(char* inodePointer) 
{
    int i;
    fileDescriptorNode* trav = fileDescriptorProcessList;
    while(trav) 
    {
        for(i = 0; i < MAX_FILES_OPEN; i++) 
        {
            if (trav->fileDescriptorTable[i].filePosition > -1) 
            {
                    if ((char*) trav->fileDescriptorTable[i].inodePointer == inodePointer) 
                    {
                        return -1;
                    }
            }
            else 
            {
                break;
            }
        }
        trav = trav->next;
    }

    return 0;
}

//pass in path and type
int create(char* pathname, char* type) {
    int parentInodeNum;
    char* fileName;
    int i;
    int freeInodeNum;
    
    dirEntry* freeDirEntry;
    freeInodeNum = 0;
    if (sb->freeInodes <= 0) 
    {
        printk("There is no freeinodes\n");
        return -1;
    }

    if (sb->freeBlocks <= 0) 
    {
        printk("There is no free blocks\n");
        return -1;
    }
    //check all parents inodes
    parentInodeNum = validateFile(pathname, type);
    //get filename
    fileName = strrchr(pathname, '/');
    fileName++;

    if (parentInodeNum > -1) 
    {
        for (i = 1; i < INODE_COUNT; i++) 
        {   //find free inode
            if (inodeArray[i].status == FREE) 
            {
                freeInodeNum = i;
                break;
            }
        }
        //update inode
        sb->freeInodes--;
        inodeArray[freeInodeNum].status = ALLOCATED;
        strcpy(inodeArray[freeInodeNum].type, type);
        inodeArray[freeInodeNum].size = 0;
        inodeArray[freeInodeNum].location[0] = getFreeBlock();
        inodeArray[freeInodeNum].locationCount = 1;
        
        //update parent by undating inode dirEntry size and dir entries
        freeDirEntry = (dirEntry*)getFreeDirEntry(parentInodeNum);
        inodeArray[parentInodeNum].size += DIR_ENTRY_STRUCTURE_SIZE;
        strcpy(freeDirEntry->fileName, fileName);

        freeDirEntry->inodeNumber = freeInodeNum;
        return 1;
    }

    return -1;
}

//regular
int ram_creat(char* pathname) 
{
    printk("create file %s\n", pathname);
    return create(pathname, "reg");
}

//dir
int ram_mkdir(char* pathname) 
{
    printk("create dir %s\n", pathname);
    return create(pathname, "dir");
}

//open file return fd
int ram_open(char* pathname) {
    char* parents;
    char* fileName;
    int parentInodeNum;
    int fileInodeNum;
    int fd;
    int pid = getpid();
    
    //check pathname
    if (strcmp(pathname, "/") == 0) 
    {
        fd = createFileDescriptor(pid, 0);  
        return fd;
    }
    //parse into parent and file name
    parse(pathname, &parents, &fileName);
    //get parent inode
    parentInodeNum = getDirInodeNumber(parents);
    
    //check parent inode
    if (parentInodeNum == -1) 
    {
        vfree(parents);
        vfree(fileName);
        return -1;
    }

    fileInodeNum = isDirEntry(parentInodeNum, fileName, "ign");
    
    //check file inode
    if (fileInodeNum == -1) 
    {
        printk("fail to open file %s\n", fileName);
        vfree(parents);
        vfree(fileName);
        return -1;
    }
    //create entry in the fdt with pid and return fd
    fd = createFileDescriptor(pid, fileInodeNum);
    return fd;
}

//close a file, remove fd, return 0 for success
int ram_close(int fd) {
    if (fd < 0) 
    {
        return -1;
    }

    fileDescriptorNode* fdClose = findFileDescriptor(fileDescriptorProcessList, getpid());

    if (fdClose == NULL) {
        return -1;
    }

    if (fdClose->fileDescriptorTable[fd].inodePointer == NULL) {
        return -1;
    }

    fdClose->fileDescriptorTable[fd].filePosition = -1;
    fdClose->fileDescriptorTable[fd].inodePointer = NULL;

    printk("succeed to close fd");
    return 0;
}

//read num_bytes from file by fd, store content in address
int ram_read(int fd, char* address, int num_bytes) {
    char* filePositionAddress;
    int fileposition, readableBytes, bytesToRead, totalBytesRead;
    fileDescriptorNode* fdRead;
    inode* inodePointer;
    //check file
    if (fd < 0) {
        return -1;
    }

    filePositionAddress = NULL;
    fdRead = findFileDescriptor(fileDescriptorProcessList, getpid());
    fileposition = readableBytes = bytesToRead = totalBytesRead = 0; 

    if (fdRead == NULL) 
    {
        printk("fail to open the file\n");
    }
    //get inode
    inodePointer = fdRead->fileDescriptorTable[fd].inodePointer;
    //only read exist bytes
    if (num_bytes > inodePointer->size) {
        num_bytes = inodePointer->size;
    }

    while (num_bytes > 0) 
    {
        fileposition = fdRead->fileDescriptorTable[fd].filePosition;
        readableBytes = mapFilepositionToMemAddr(inodePointer, fileposition, &filePositionAddress);
        bytesToRead = getMin(readableBytes, num_bytes);

        memcpy(address, filePositionAddress, bytesToRead);

        num_bytes -= bytesToRead;
        totalBytesRead += bytesToRead;
        //update file position
        fdRead->fileDescriptorTable[fd].filePosition += bytesToRead;
        filePositionAddress = NULL;
    }
    return totalBytesRead;
}

//write num_bytes into file by fd
int ram_write(int fd, char *address, int num_bytes) 
{
    char* filePositionAddress;
    int totalBytesWritten;
    fileDescriptorNode* fdWrite;
    int fileposition;
    int writeableBytes;
    char* ret;
    int bytesToWrite;
    inode* inodePointer;


    if (fd < 0) 
    {
        return -1;
    }

    fdWrite = findFileDescriptor(fileDescriptorProcessList, getpid());
    filePositionAddress = NULL;
    totalBytesWritten = 0;

    if (fdWrite == NULL)
    {
        return -1;
    }
    
    if (fdWrite->fileDescriptorTable[fd].inodePointer == NULL) 
    {
        return -1;
    }

    inodePointer = fdWrite->fileDescriptorTable[fd].inodePointer;


    while (num_bytes > 0) 
    {
        fileposition = fdWrite->fileDescriptorTable[fd].filePosition;
       //add block if more than file size
        if ((fileposition % RD_BLOCK_SIZE == 0) && (fileposition == inodePointer->size) && (inodePointer->size != 0)) 
        {
            if (fileposition == MAX_FILE_SIZE) 
            {
                return -1;
            }
            ret = allocateBlock(inodePointer);
            if (!ret) 
            {
                return -1;
            }
        }

        writeableBytes = mapFilepositionToMemAddr(inodePointer, fileposition, &filePositionAddress);
        bytesToWrite = min(writeableBytes, num_bytes);

        if (filePositionAddress == NULL) {
            return -1;
        }

        memcpy(filePositionAddress, address, bytesToWrite);
        num_bytes -= bytesToWrite;
        totalBytesWritten += bytesToWrite;
        //update
        fdWrite->fileDescriptorTable[fd].filePosition += bytesToWrite;
        inodePointer->size += bytesToWrite;

        filePositionAddress = NULL;
    }

    return totalBytesWritten;
}

//seek to the offset in a file by fd
int ram_lseek(int fd, int offset) 
{
    fileDescriptorNode* fdSeek;
    int fileSize;
    //check
    if (fd < 0) 
    {
        return -1;
    }

    fdSeek = findFileDescriptor(fileDescriptorProcessList, getpid());

    if (fdSeek == NULL) 
    {
        printk("fail to seek the file");
        return -1;
    }
    //check if the calling process opened the file
    if (fdSeek->fileDescriptorTable[fd].inodePointer == NULL) 
    {
        printk("fail to seek the file");
        return -1;
    }

    if (strcmp(fdSeek->fileDescriptorTable[fd].inodePointer->type, "dir") == 0) 
    {
        printk("fail to seek the file");
        return -1;
    }

    fileSize = fdSeek->fileDescriptorTable[fd].inodePointer->size;
    //check offset
    if (offset < 0) 
    {
        offset = 0;
    }

    if (offset > fileSize) 
    {
        fdSeek->fileDescriptorTable[fd].filePosition = fileSize;
    }

    else 
    {
        fdSeek->fileDescriptorTable[fd].filePosition = offset;
    }

    return 0;
}

//remove file by pathname
int ram_unlink(char* pathname) {
    char* parents;
    char* fileName;
    int parentInodeNum;
    int fileInodeNum;
    char* inodePointer;
    char* fileType;
    int deletedInodeNum;
    int locationCount, directCount, i;
        singleIndirectLevel* singleIndirectBlock;
        char* unlinkEntryIter;
        doubleIndirectLevel* doubleIndirectBlock;
    //check if root
    if (strcmp(pathname, "/") == 0) 
    {
        printk("fail to unlink root dir\n");
        return -1;
    }
    //parse into parent and file name
    parse(pathname, &parents, &fileName);
    //get parent inode
    parentInodeNum = getDirInodeNumber(parents);

    fileInodeNum = isDirEntry(parentInodeNum, fileName, "ign");
    inodePointer = (char*) &inodeArray[fileInodeNum];
    
   
    fileType = inodeArray[fileInodeNum].type;
     //unlink non-empty directory
    if (strcmp(fileType, "dir") == 0 && inodeArray[fileInodeNum].size != 0) 
    {
        printk("fail to unlink: the dir is not null\n");
        return -1;
    }
    //delete entry from parent
    deletedInodeNum = unlinkHelper(parentInodeNum, fileName, inodeArray[fileInodeNum].type);

    inodeArray[parentInodeNum].size -= DIR_ENTRY_STRUCTURE_SIZE;
    sb->freeInodes += 1;
    
    inodeArray[deletedInodeNum].inodeNumber = deletedInodeNum;
    inodeArray[deletedInodeNum].status = FREE;
    inodeArray[deletedInodeNum].size = 0;
    strcpy(inodeArray[deletedInodeNum].type, "nil");
    //get all blocks
    locationCount = inodeArray[deletedInodeNum].locationCount;
    if (locationCount > 8)
    {
        directCount = 8;
    }
    else
    {
        directCount = locationCount;
    }

    for(i = 0; i < directCount; i++) 
    {
        if (inodeArray[deletedInodeNum].location[i] == NULL)
        {
            break;
        }

        memset(inodeArray[deletedInodeNum].location[i], 0, RD_BLOCK_SIZE);
        setBitmap(inodeArray[deletedInodeNum].location[i]);
    }
    //single indirect
    if (locationCount > 8) 
    {
        singleIndirectBlock = (singleIndirectLevel*) inodeArray[deletedInodeNum].location[8];

        for (i = 0; i < 64; i++) 
        {
            unlinkEntryIter = singleIndirectBlock->pointers[i];

            if (!unlinkEntryIter) 
            {
                break;
            }

            memset(unlinkEntryIter, 0, RD_BLOCK_SIZE);
            setBitmap(unlinkEntryIter);
        }
    }
    //double indirect
    if (locationCount == 10) 
    {
        doubleIndirectBlock = (doubleIndirectLevel*) inodeArray[deletedInodeNum].location[9];

        for (i = 0; i < 64; i++) 
        {
            singleIndirectBlock = doubleIndirectBlock->pointers[i];

            if (!singleIndirectBlock) 
            {
                break;
            }

            for (i = 0; i < 64; i++) 
            {
                unlinkEntryIter = singleIndirectBlock->pointers[i];

                if (!unlinkEntryIter) 
                {
                    break;
                }

                memset(unlinkEntryIter, 0, RD_BLOCK_SIZE);        
                setBitmap(unlinkEntryIter);
            }
        }
    }
    
    inodeArray[deletedInodeNum].locationCount = 0;

    printk("unlink %s\n", pathname);
    return 0;
}

int ram_readdir(int fd, char* address) 
{
    char* filePositionMemAddress;
    int ret;
    fileDescriptorNode* fdReadDir;
    fileDescriptorEntry fd_entry;
    int filePosition;
    inode* inodePointer;

        
    if (fd < 0) 
    {
        return -1;
    }

    fdReadDir = findFileDescriptor(fileDescriptorProcessList, getpid());
    fd_entry = fdReadDir->fileDescriptorTable[fd];
    filePosition = fd_entry.filePosition;

    inodePointer = fd_entry.inodePointer;

    if (inodePointer->size == 0) 
    {
        printk("there is no file in the dir\n");
        return 0;
    }

    if (filePosition == inodePointer->size) 
    {
        printk("fail to read the dir\n");
        return 0;
    }
    
    ret = mapFilepositionToMemAddr(inodePointer, filePosition, &filePositionMemAddress);

    memcpy(address, filePositionMemAddress, DIR_ENTRY_STRUCTURE_SIZE);
    fdReadDir->fileDescriptorTable[fd].filePosition += DIR_ENTRY_STRUCTURE_SIZE;

    return 1;
}


// ioctl for the ramdisk 
static int ramdisk_ioctl(struct inode *inode, struct file *file, 
                         unsigned int cmd, unsigned long arg) 
{
    ioctl_rd params;
    char* path;
    char* kernelAddress;
    int size;
    int ret;
    path = NULL;

    copy_from_user(&params, (ioctl_rd *)arg, sizeof(ioctl_rd));

    switch (cmd) 
    {
        case IOCTL_RD_CREAT://create 
            size = sizeof(char) * (params.pathnameLength + 1);
            path = (char*)vmalloc(size);
            copy_from_user(path, params.pathname, size);
    
                        ret = ram_creat(path);
            vfree(path);
            return ret;
            break;
   
        case IOCTL_RD_MKDIR://mkdir
            size = sizeof(char) * (params.pathnameLength + 1);
            path = (char*)vmalloc(size);
            copy_from_user(path, params.pathname, size);
                        
            ret = ram_mkdir(path);
            vfree(path);
            return ret;
            break;
    
        case IOCTL_RD_OPEN://open
            size = sizeof(char) * (params.pathnameLength + 1);
            path = (char*)vmalloc(size);
            copy_from_user(path, params.pathname, size);
                        ret = ram_open(path);
            vfree(path);
            return ret;
            break;
        
        case IOCTL_RD_CLOSE://close
            ret = ram_close(params.fd);
            return ret;
            break;

        case IOCTL_RD_READ://read
            kernelAddress = (char*) vmalloc(params.num_bytes);
            ret = ram_read(params.fd, kernelAddress, params.num_bytes);
            if (ret != -1) 
            {
                copy_to_user(params.address, kernelAddress, ret);
        	}
            vfree(kernelAddress);

            return ret;
            break;
    
        case IOCTL_RD_WRITE://write
            kernelAddress = (char*) vmalloc(params.num_bytes);
            copy_from_user(kernelAddress, params.address, (unsigned long) params.num_bytes);
            ret = ram_write(params.fd, kernelAddress, params.num_bytes);
            vfree(kernelAddress);
            return ret;
            break;
    
        case IOCTL_RD_LSEEK://lseek
            ret = ram_lseek(params.fd, params.offset);
            return ret;
            break;

        case IOCTL_RD_UNLINK://unlink
            size = sizeof(char) * (params.pathnameLength + 1);
            path = (char*)vmalloc(size);
            copy_from_user(path, params.pathname, size);

            ret = ram_unlink(path);
            vfree(path);
            return ret;
            break;

        case IOCTL_RD_READDIR://readdir
            kernelAddress = (char*) vmalloc(16);
            ret = ram_readdir(params.fd, kernelAddress);
            copy_to_user(params.address, kernelAddress, 16);
            vfree(kernelAddress);
            return ret;
            break;

        default:
            return -EINVAL;
            break;
  }
  return 0;
}

static int __init init_ramdisk(void) {
    int ret;

    ramdiskOperations.ioctl = ramdisk_ioctl;

    proc_entry = create_proc_entry("ramdisk_ioctl", 0444, NULL);
    proc_backup = create_proc_entry("ramdisk_backup", 0644, NULL);
    proc_entry->proc_fops = &ramdiskOperations;

    ret = initRamdisk();

    return ret;
}

static void __exit exit_ramdisk(void) 
{
    destroyRamdisk();

    remove_proc_entry("ramdisk_ioctl", NULL);
    remove_proc_entry("ramdisk_backup", NULL);

    return;
}

module_init(init_ramdisk);
module_exit(exit_ramdisk);
