/* -*-C-*-
 *******************************************************************************
 *
 * File:         sfs.c
 * RCS:          $Id: sfs.c,v 1.2 2009/11/10 21:17:25 npb853 Exp $
 * Description:  Simple File System
 * Author:       Fabian E. Bustamante
 *               Northwestern Systems Research Group
 *               Department of Computer Science
 *               Northwestern University
 * Created:      Tue Nov 05, 2002 at 07:40:42
 * Modified:     Fri Nov 19, 2004 at 15:45:57 fabianb@cs.northwestern.edu
 * Language:     C
 * Package:      N/A
 * Status:       Experimental (Do Not Distribute)
 *
 * (C) Copyright 2003, Northwestern University, all rights reserved.
 *
 *******************************************************************************
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>
 
#include "sfs.h"
#include "sdisk.h"

typedef struct inode_s {
	bool isFile;
	struct inode_s* parent;
	struct inode_s* cont;
	char name[16];
} inode;

typedef struct inodeFile_s {
	bool isFile;
	inode* parent;
	inode* cont;
	char name[16];
	int sectors[6];
	int filesize;
} inodeFile;

typedef struct inodeDir_s {
	bool isFile;
	inode* parent;
	inode* cont;
	char name[16];
	int children[6];
} inodeDir;

typedef struct fileDescriptor_s {
	inode* INODE;
	int currentPos;
	char* data;
	struct fileDescriptor_s* next;
} fileDescriptor;

static int sectorBitmapSizeInSectors = -1;
static int inodeBitmapSizeInSectors = -1;
static int inodeArraySizeInSectors = -1;

static int cwd = -1;

// some static file containing info on all open files

static short DEBUG = 0;

static int inodeSize = sizeof(inodeFile) > sizeof(inodeDir) ? sizeof(inodeFile) : sizeof(inodeDir);

Sector* getSector(int);

void markSectorAsUsed(int);
void markSectorAsNotUsed(int);

void markInodeAsUsed(int);
void markInodeAsNotUsed(int);

int getNextFreeInode();

int createInode();
inode* getInode(int);

void initDir(inodeDir*, inode*, inode*, char[16]);
void initFile(inodeFile*, inode*, inode*, char[16]);
void initInode(inode*, bool, inode*, inode*, char[16]);

void setBit(int*, int);
void clearBit(int*, int);
void toggleBit(int*, int);
int getBit(int*, int);
void initSector(int);

Sector* getSector(int sector) {
	Sector* retrievedSector = malloc(sizeof(Sector));
	SD_read(sector, retrievedSector);
	return retrievedSector;
}

// Mark a sector as in use in our bitmap
void markSectorAsUsed(int sector) {
	int bitmapSectorNumber = floor( sector / SD_SECTORSIZE );
	int sectorOffset = sector % SD_SECTORSIZE;
	
	Sector* bitmapSector = getSector(bitmapSectorNumber);
	
	int* intToInspect = (int*)bitmapSector + (int)(floor( sectorOffset / 32 ));
	
	setBit(intToInspect, sectorOffset % 32);
	SD_write(bitmapSectorNumber, bitmapSector);
	
	free(bitmapSector);
}

// Mark a sector as not in use in our bitmap
void markSectorAsNotUsed(int sector) {
	int bitmapSectorNumber = floor( sector / SD_SECTORSIZE );
	int sectorOffset = sector % SD_SECTORSIZE;
	
	Sector* bitmapSector = getSector(bitmapSectorNumber);
	
	int* intToInspect = (int*)bitmapSector + (int)(floor( sectorOffset / 32 ));
	
	clearBit(intToInspect, sectorOffset % 32);
	SD_write(bitmapSectorNumber, bitmapSector);
	
	free(bitmapSector);
}

void markInodeAsUsed(int inodeNumber) {
	int inodeSectorNumber = floor( inodeNumber / SD_SECTORSIZE ) + sectorBitmapSizeInSectors;
	int sectorOffset = inodeNumber % SD_SECTORSIZE;
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	
	int* intToInspect = (int*)inodeSector + (int)(floor( sectorOffset / 32 ));
	
	setBit(intToInspect, sectorOffset % 32);
	SD_write(inodeSectorNumber, inodeSector);
	
	free(inodeSector);
}

void markInodeAsNotUsed(int inodeNumber) {
	int inodeSectorNumber = floor( inodeNumber / SD_SECTORSIZE ) + sectorBitmapSizeInSectors;
	int sectorOffset = inodeNumber % SD_SECTORSIZE;
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	
	int* intToInspect = (int*)inodeSector + (int)(floor( sectorOffset / 32 ));
	
	clearBit(intToInspect, sectorOffset % 32);
	SD_write(inodeSectorNumber, inodeSector);
	
	free(inodeSector);
}

int getNextFreeInode() {
	int fullIntegerBitmap = ~0;
	int value = 0;
	
	int secNum = sectorBitmapSizeInSectors;
	
	Sector* bitmap = getSector(secNum);
	int* curPos = (int*)bitmap;
	
	if (DEBUG) printf("Our bitmap is %i, ", *(int*)bitmap);
	
	while (*(int*)curPos == fullIntegerBitmap) {
		if (DEBUG) printf("we need a new int boundary, ");
		value += 32;
		if (DEBUG) printf("our bitmap pointer goes from %p ", curPos);
		curPos++;
		if (DEBUG) printf("to %p, where the value of curPos is %i, ", curPos, *(int*)curPos);
		
		if (value % sizeof(Sector) == 0) {
			secNum++;
			free(bitmap);
			bitmap = getSector(secNum);
			curPos = (int*)bitmap;
		}
	}
	
	while ((*(int*)curPos & 1) != 0) {
		value++;
		*(int*)curPos >>= 1;
	}
	
	free(bitmap);
	
	if (DEBUG) printf("and our next free one is %i\n", value);
	
	return value;
}

int createInode() {
	int inodeNum = getNextFreeInode();
	markInodeAsUsed(inodeNum);
	return inodeNum;
}

inode* getInode(int inodeNum) {
	int inodesPerSector = ceil( (double)SD_SECTORSIZE / (double)inodeSize );
	int inodeSectorNumber = floor( (double)inodeNum / (double)inodesPerSector ) + sectorBitmapSizeInSectors + inodeBitmapSizeInSectors;
	Sector* inodeSector = getSector(inodeSectorNumber);
	Sector* inodeList = inodeSector;
	int inodeOffset = inodeNum % inodesPerSector;
	int byteOffset = inodeOffset * inodeSize;
	inodeList += byteOffset / sizeof(Sector);
	free(inodeSector);
	return (inode*)inodeList;
}

void initDir(inodeDir* dir, inode* parent, inode* cont, char name[16]) {
	int i;
	for (i = 0; i < 6; ++i) {
		dir->children[i] = 0;
	}
	initInode((inode*) dir, 0, parent, cont, name);
}
void initFile(inodeFile* file, inode* parent, inode* cont, char name[16]) {
	int i;
	for (i = 0; i < 6; ++i) {
		file->sectors[i] = -1;
	}
	initInode((inode*) file, 0, parent, cont, name);
}
void initInode(inode* INODE, bool isFile, inode* parent, inode* cont, char name[16]) {
	INODE->isFile = isFile;
	INODE->parent = parent;
	INODE->cont = cont;
	strcpy(INODE->name, name);
}

void setBit(int* sequence, int bitNum) {
	*sequence |= 1 << bitNum;
}

void clearBit(int* sequence, int bitNum) {
	*sequence &= ~(1 << bitNum);
}

int getBit(int* sequence, int bitNum) {
	return *sequence & (1 << bitNum);
}

void toggleBit(int* sequence, int bitNum) {
	*sequence ^= 1 << bitNum;
}

void initSector(int sector) {
	Sector* bitmapSector = malloc(sizeof(Sector));
	int* intBoundary = (int*)bitmapSector;
	SD_read(sector, bitmapSector);
	int i;
	for (i = 0; i < SD_SECTORSIZE / sizeof(int); i++) {
		*intBoundary = 0;
		intBoundary++;
	}
	*(int*)bitmapSector = 0;
	SD_write(sector, bitmapSector);
	free(bitmapSector);
}

/*
 * sfs_mkfs: use to build your filesystem
 *
 * Parameters: -
 *
 * Returns: 0 on success, or -1 if an error occurred
 *
 */
int sfs_mkfs() {
	int numBytes = ceil( (double)SD_NUMSECTORS / (double)8 );
	sectorBitmapSizeInSectors = ceil( (double)numBytes / (double)SD_SECTORSIZE );
	
	int numInodes = SD_NUMSECTORS - sectorBitmapSizeInSectors;
	inodeBitmapSizeInSectors = ceil( (double)numInodes / (double)SD_SECTORSIZE / (double)8 );
	
	inodeArraySizeInSectors = ceil( (double)numInodes * (double)inodeSize / (double)SD_SECTORSIZE );
	
	int i;
	for(i = 0; i < sectorBitmapSizeInSectors + inodeBitmapSizeInSectors + inodeArraySizeInSectors; ++i) {
		initSector(i);
		markSectorAsUsed(i);
	}
	
	//printf("Our sector bitmap is %i sectors, there are %i inodes, our inode bitmap is %i sectors, and our inode list is %i sectors\n", sectorBitmapSizeInSectors, numInodes, inodeBitmapSizeInSectors, inodeArraySizeInSectors);
	
	cwd = createInode(); // initialize current working directory to root inode
	
	inodeDir* rootInode = (inodeDir*)getInode(cwd);
	initDir(rootInode, 0, 0, "");
	
    return 0;
} /* !sfs_mkfs */

/*
 * sfs_mkdir: attempts to create the name directory
 *
 * Parameters: directory name
 *
 * Returns: 0 on success, or -1 if an error occurred
 *
 */
int sfs_mkdir(char *name) {
    // parse the name to get the parents and the name of the new directory
	// iterate over the inodes until you find the subdirectory you're supposed to create it at
	// create it
    return -1;
} /* !sfs_mkdir */

/*
 * sfs_fcd: attempts to change current directory to named directory
 *
 * Parameters: new directory name
 *
 * Returns: 0 on success, or -1 if an error occurred
 *
 */
int sfs_fcd(char* name) {
	// parse the path and iterate through all the inodes until we find the new one
    return -1;
} /* !sfs_fcd */

/*
 * sfs_ls: output the information of all existing files in 
 *   current directory
 *
 * Parameters: -
 *
 * Returns: 0 on success, or -1 if an error occurred
 *
 */
int sfs_ls(FILE* f) {
    inodeDir* cwi = (inodeDir*)getInode(cwd);
	do {
		int i;
		for (i = 0; i < 6; ++i) {
			if (cwi->children[i] != 0) {
				fprintf(f, "%s\n", getInode(cwi->children[i])->name);
			}
		}
	} while( (cwi = (inodeDir*)cwi->cont) != 0);
    return 0;
} /* !sfs_ls */

/*
 * sfs_fopen: convert a pathname into a file descriptor. When the call
 *   is successful, the file descriptor returned will be the lowest file
 *   descriptor not currently open for the process. If the file does not
 *   exist it will be created.
 *
 * Parameters: file name
 *
 * Returns:  return the new file descriptor, or -1 if an error occurred
 *
 */
int sfs_fopen(char* name) {
    // find the inode based on the name
	// write file info to our file descriptors array
	// and return the index into the array
    return -1;
} /* !sfs_fopen */

/*
 * sfs_fclose: close closes a file descriptor, so that it no longer
 *   refers to any file and may be reused.
 *
 * Parameters: -
 *
 * Returns: 0 on success, or -1 if an error occurred
 *
 */
int sfs_fclose(int fileID) {
    // write any changes in the data to the disk
	// write any changes in the inode to the disk
	// delete the file descriptor
    return -1;
} /* !sfs_fclose */

/*
 * sfs_fread: attempts to read up to length bytes from file
 *   descriptor fileID into the buffer starting at buffer
 *
 * Parameters: file descriptor, buffer to read and its lenght
 *
 * Returns: on success, the number of bytes read are returned. On
 *   error, -1 is returned
 *
 */
int sfs_fread(int fileID, char *buffer, int length) {
    // get the file descriptor
	// find out which sectors comprise that file
	// copy the length bytes from the sectors into buffer
    return -1;
}

/*
 * sfs_fwrite: writes up to length bytes to the file referenced by
 *   fileID from the buffer starting at buffer
 *
 * Parameters: file descriptor, buffer to write and its lenght
 *
 * Returns: on success, the number of bytes written are returned. On
 *   error, -1 is returned
 *
 */
int sfs_fwrite(int fileID, char *buffer, int length) {
    // get the file descriptor
	// find out which sectors comprise that file
	// copy length bytes from the buffer into the sectors
    return -1;
} /* !sfs_fwrite */

/*
 * sfs_lseek: reposition the offset of the file descriptor 
 *   fileID to position
 *
 * Parameters: file descriptor and new position
 *
 * Returns: Upon successful completion, lseek returns the resulting
 *   offset location, otherwise the value -1 is returned
 *
 */
int sfs_lseek(int fileID, int position) {
    // get the file descriptor
	// change the offset to position
    return -1;
} /* !sfs_lseek */

/*
 * sfs_rm: removes a file in the current directory by name if it exists.
 *
 * Parameters: file name
 *
 * Returns: 0 on success, or -1 if an error occurred
 */
int sfs_rm(char *file_name) {
    // TODO: Implement for extra credit
    return -1;
} /* !sfs_rm */
