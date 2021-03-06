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
#include <sys/param.h>
 
#include "sfs.h"
#include "sdisk.h"

typedef struct inode_s {
	bool isFile;
	int parent;
	int cont;
	char name[16];
	int num;
} inode;

typedef struct inodeFile_s {
	bool isFile;
	int parent;
	int cont;
	char name[16];
	int num;
	int sectors[6];
	int filesize;
} inodeFile;

typedef struct inodeDir_s {
	bool isFile;
	int parent;
	int cont;
	char name[16];
	int num;
	int children[6];
} inodeDir;

typedef struct fileDescriptor_s {
	int num;
	inode* INODE;
	int curPos;
	char* data;
	struct fileDescriptor_s* next;
} fileDescriptor;

typedef struct tokenResult_s {
	int numTokens;
	char** tokens;
} tokenResult;

static int sectorBitmapSizeInSectors = -1;
static int inodeBitmapSizeInSectors = -1;
static int inodeArraySizeInSectors = -1;

static int rootInodeNum = -1;

static int cwd = -1;

static fileDescriptor* fdList = NULL;

static short DEBUG = 0;

static int inodeSize = sizeof(inodeFile) > sizeof(inodeDir) ? sizeof(inodeFile) : sizeof(inodeDir);

Sector* getSector(int);

void markSectorAsUsed(int);
void markSectorAsNotUsed(int);

void markInodeAsUsed(int);
void markInodeAsNotUsed(int);

int getNextFreeInode();

int getNextFreeSector();
int useNextFreeSector();

int createInode();
inode* getInode(int);
void saveInode(inode*);

void initDir(inodeDir*, int, int, int, char[16]);
void initFile(inodeFile*, int, int, int, char[16]);
void initInode(inode*, int, bool, int, int, char[16]);

void setBit(int*, int);
void clearBit(int*, int);
void toggleBit(int*, int);
int getBit(int*, int);
void initSector(int);

tokenResult* parsePath(char*);
void freeToken(tokenResult*);

void addChild(inodeDir*, int);

inode* getCont(inode*);

fileDescriptor* removeFd(int);
void deleteFd(int);

fileDescriptor* getNewFd();
int createFd(inode*);

fileDescriptor* findFd(int);

void writeFdToDisk(int);

int countSectorsInFile(inodeFile*);
void addSector(inodeFile*);

Sector* getSector(int sector) {
	Sector* retrievedSector = malloc(sizeof(Sector));
	while (SD_read(sector, retrievedSector) != 0) {}
	return retrievedSector;
}

// Mark a sector as in use in our bitmap
void markSectorAsUsed(int sector) {
	int bitmapSectorNumber = floor( sector / (SD_SECTORSIZE * 8) );
	int sectorOffset = sector % (SD_SECTORSIZE * 8);
	
	Sector* bitmapSector = getSector(bitmapSectorNumber);
	
	int* intToInspect = (int*)bitmapSector + (int)(floor( sectorOffset / 32 ));
	
	setBit(intToInspect, sectorOffset % 32);
	while ( SD_write(bitmapSectorNumber, bitmapSector) != 0 ) {}
	
	free(bitmapSector);
}

// Mark a sector as not in use in our bitmap
void markSectorAsNotUsed(int sector) {
	int bitmapSectorNumber = floor( sector / (SD_SECTORSIZE * 8) );
	int sectorOffset = sector % (SD_SECTORSIZE * 8);
	
	Sector* bitmapSector = getSector(bitmapSectorNumber);
	
	int* intToInspect = (int*)bitmapSector + (int)(floor( sectorOffset / 32 ));
	
	clearBit(intToInspect, sectorOffset % 32);
	while ( SD_write(bitmapSectorNumber, bitmapSector) != 0 ) {}
	
	free(bitmapSector);
}

void markInodeAsUsed(int inodeNumber) {
	int inodeSectorNumber = floor( inodeNumber / (SD_SECTORSIZE * 8) ) + sectorBitmapSizeInSectors;
	int sectorOffset = inodeNumber % (SD_SECTORSIZE * 8);
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	
	int* intToInspect = (int*)inodeSector + (int)(floor( sectorOffset / 32 ));
	
	setBit(intToInspect, sectorOffset % 32);
	while ( SD_write(inodeSectorNumber, inodeSector) != 0 ) {}
	
	free(inodeSector);
}

void markInodeAsNotUsed(int inodeNumber) {
	int inodeSectorNumber = floor( inodeNumber / (SD_SECTORSIZE * 8) ) + sectorBitmapSizeInSectors;
	int sectorOffset = inodeNumber % (SD_SECTORSIZE * 8);
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	
	int* intToInspect = (int*)inodeSector + (int)(floor( sectorOffset / 32 ));
	
	clearBit(intToInspect, sectorOffset % 32);
	while ( SD_write(inodeSectorNumber, inodeSector) != 0 ) {}
	
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
		
		if (value / 8 % sizeof(Sector) == 0) {
			if (DEBUG) printf("we need a new sector, ");
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

int getNextFreeSector() {
	int fullIntegerBitmap = ~0;
	int value = 0;
	
	int secNum = 0;
	
	Sector* bitmap = getSector(secNum);
	int* curPos = (int*)bitmap;
	
	if (DEBUG) printf("Our bitmap is %i, ", *(int*)bitmap);
	
	while (*(int*)curPos == fullIntegerBitmap) {
		if (DEBUG) printf("we need a new int boundary, ");
		value += 32;
		if (DEBUG) printf("our bitmap pointer goes from %p ", curPos);
		curPos++;
		if (DEBUG) printf("to %p, where the value of curPos is %i, ", curPos, *(int*)curPos);
		
		if (value / 8 % sizeof(Sector) == 0) {
			if (DEBUG) printf("we need a new sector, ");
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

int useNextFreeSector() {
	int sector = getNextFreeSector();
	markSectorAsUsed(sector);
	return sector;
}

int createInode() {
	int inodeNum = getNextFreeInode();
	markInodeAsUsed(inodeNum);
	return inodeNum;
}

inode* getInode(int inodeNum) {
	int inodesPerSector = floor( (double)SD_SECTORSIZE / (double)inodeSize );
	int inodeSectorNumber = floor( (double)inodeNum / (double)inodesPerSector ) + sectorBitmapSizeInSectors + inodeBitmapSizeInSectors;
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	char* inodeList = (char*)inodeSector;
	
	int inodeOffset = inodeNum % inodesPerSector;
	int byteOffset = inodeOffset * inodeSize;
	inodeList += byteOffset;
	
	inode* resultInode = malloc(inodeSize);
	memcpy(resultInode, inodeList, inodeSize);
	
	free(inodeSector);
	return resultInode;
}

void saveInode(inode* INODE) {
	int inodeNum = INODE->num;
	int inodesPerSector = floor( (double)SD_SECTORSIZE / (double)inodeSize );
	int inodeSectorNumber = floor( (double)inodeNum / (double)inodesPerSector ) + sectorBitmapSizeInSectors + inodeBitmapSizeInSectors;
	
	Sector* inodeSector = getSector(inodeSectorNumber);
	char* inodeList = (char*)inodeSector;
	
	int inodeOffset = inodeNum % inodesPerSector;
	int byteOffset = inodeOffset * inodeSize;
	
	inodeList += byteOffset;
	
	inode* inMem = (inode*)INODE;
	inode* onDisk = (inode*)inodeList;
	
	onDisk->isFile = inMem->isFile;
	onDisk->parent = inMem->parent;
	onDisk->cont = inMem->cont;
	
	strcpy(onDisk->name, inMem->name);
	
	onDisk->num = inMem->num;
	
	if (DEBUG) printf("Is the inode a file? %i\n", INODE->isFile);
	
	if (INODE->isFile) {
		inodeFile* inMemFile = (inodeFile*)INODE;
		inodeFile* onDiskFile = (inodeFile*)inodeList;
		
		if (DEBUG) printf("The filesize is %i\n", inMemFile->filesize);
		memcpy(onDiskFile->sectors, inMemFile->sectors, sizeof(int)*6);
		onDiskFile->filesize = inMemFile->filesize;
	} else {
		inodeDir* inMemDir = (inodeDir*)INODE;
		inodeDir* onDiskDir = (inodeDir*)inodeList;
		
		memcpy(onDiskDir->children, inMemDir->children, sizeof(int)*6);
	}
	
	Sector* inodeSectorCopy = inodeSector;
	while ( SD_write(inodeSectorNumber, inodeSectorCopy) != 0 ) {}
	
	free(inodeSector);
}

void initDir(inodeDir* dir, int inodeNum, int parent, int cont, char name[16]) {
	int i;
	for (i = 0; i < 6; ++i) {
		dir->children[i] = -1;
	}
	initInode((inode*) dir, inodeNum, 0, parent, cont, name);
}
void initFile(inodeFile* file, int inodeNum, int parent, int cont, char name[16]) {
	int i;
	for (i = 0; i < 6; ++i) {
		file->sectors[i] = -1;
	}
	initInode((inode*) file, inodeNum, 1, parent, cont, name);
}
void initInode(inode* INODE, int inodeNum, bool isFile, int parent, int cont, char name[16]) {
	INODE->isFile = isFile;
	INODE->parent = parent;
	INODE->cont = cont;
	strcpy(INODE->name, name);
	INODE->num = inodeNum;
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
	while ( SD_read(sector, bitmapSector) != 0 ) {}
	int i;
	for (i = 0; i < SD_SECTORSIZE / sizeof(int); i++) {
		*intBoundary = 0;
		intBoundary++;
	}
	*(int*)bitmapSector = 0;
	while ( SD_write(sector, bitmapSector) != 0 ) {}
	free(bitmapSector);
}

tokenResult* parsePath(char* path) {

	char* pathCopy = malloc(sizeof(char) * 20);
	
	strcpy(pathCopy, path);
	tokenResult* tokens = malloc(sizeof(tokenResult));
	tokens->tokens = malloc(MAXPATHLEN);
	char* token;
	token = strtok(pathCopy, "/");
	int numTokens = 0;
	while (token != NULL) {
		tokens->tokens[numTokens] = (char*)malloc(sizeof(char) * 17);
		strcpy(tokens->tokens[numTokens], token);
		numTokens++;
		token = strtok(NULL, "/");
	}
	
	free(pathCopy);
	tokens->numTokens = numTokens;
	return tokens;
}

void freeToken(tokenResult* tokens) {
	int i;
	for (i = 0; i < tokens->numTokens; ++i) {
		free(tokens->tokens[i]);
	}
	free(tokens->tokens);
	free(tokens);
}

void addChild(inodeDir* parent, int childNum) {
	int i;
	bool added = 0;
	for (i = 0; i < 6; i++) {
		if (parent->children[i] == -1) {
			added = 1;
			parent->children[i] = childNum;
			break;
		}
	}
	if (!added) {
		if (DEBUG) printf("Checking if we need to add a continuing inode (%i) to %i...\n", parent->cont, parent->num);
		if (parent->cont == -1) {
			int contInodeNum = createInode();
			inodeDir* contInode = (inodeDir*)getInode(contInodeNum);
			initDir(contInode, contInodeNum, -1, -1, "");
			parent->cont = contInodeNum;
			saveInode((inode*)contInode);
			free(contInode);
		}
		if (DEBUG) printf("Adding the child to the continuing inode...\n");
		inode* contInode = getInode(parent->cont);
		addChild((inodeDir*)contInode, childNum);
		saveInode(contInode);
		free(contInode);
	}
}

inode* getCont(inode* INODE) {
	if (INODE->cont == -1) {
		return 0;
	}
	int contInodeNum = INODE->cont;
	free(INODE);
	return getInode(contInodeNum);
}

fileDescriptor* removeFd(int fdNum) {
	fileDescriptor* fd = fdList;
	if (fd == NULL) {
		return NULL;
	}
	if (fd->num == fdNum) {
		fdList = fdList->next;
		return fd;
	}
	while (fd->next != NULL) {
		if (fd->next->num == fdNum) {
			fd->next = fd->next->next;
			return fd->next;
		}
		fd = fd->next;
	}
	return NULL;
}

void deleteFd(int fdNum) {
	fileDescriptor* fd = removeFd(fdNum);
	if (fd != NULL) {
		free(fd->INODE);
		free(fd->data);
		free(fd);
	}
}

fileDescriptor* getNewFd() {
	fileDescriptor* fd = malloc(sizeof(fileDescriptor));
	if (fdList != NULL) {
		fd->num = fdList->num + 1;
		fd->next = fdList;
	} else {
		fd->num = 1;
		fd->next = NULL;
	}
	fdList = fd;
	return fd;
}

int createFd(inode* INODE) {
	inodeFile* inodeF = (inodeFile*)INODE;
	if (DEBUG) printf("Get a new file descriptor\n");
	fileDescriptor* fd = getNewFd();
	fd->INODE = INODE;
	fd->curPos = 0; // should this be filesize instead?
	int numSectors = ceil( (double)inodeF->filesize / (double)SD_SECTORSIZE );
	int dataSize = (int)(SD_SECTORSIZE * numSectors);
	if (DEBUG) printf("Malloc space for %i sectors (%i bytes) based on a filesize of %i\n", numSectors, dataSize, inodeF->filesize);
	fd->data = malloc(dataSize);
	char* curPos = fd->data;
	inodeFile* workingDirCont = malloc(inodeSize);
	memcpy(workingDirCont, inodeF, inodeSize);
	if (DEBUG) printf("Copy the data over (there should be %i sectors, and we're allocating %i bytes)\n", countSectorsInFile((inodeFile*)fd->INODE), dataSize);
	int copied = 0;
	do {
		int i;
		if (DEBUG) printf("Going one level down\n");
		for (i = 0; i < 6; ++i) {
			if (workingDirCont->sectors[i] != -1) {
				if (DEBUG) printf("Copying a child to %p (copy #%i)\n", curPos, ++copied);
				Sector* dataSector = getSector(workingDirCont->sectors[i]);
				memcpy(curPos, dataSector, SD_SECTORSIZE);
				if (DEBUG) printf("Trying to free %p\n", dataSector);
				free(dataSector);
				if (DEBUG) printf("Done freeing\n");
				curPos += SD_SECTORSIZE;
			}
		}
	} while( (workingDirCont = (inodeFile*)getCont((inode*)workingDirCont)) != 0);
	if (DEBUG) printf("Finished copying data\n");
	return fd->num;
}

fileDescriptor* findFd(int fdNum) {
	fileDescriptor* fd = fdList;
	while (fd != NULL && fd->num != fdNum) {
		fd = fd->next;
	}
	return fd;
}

void writeFdToDisk(int fdNum) {
	fileDescriptor* fd = findFd(fdNum);
	saveInode(fd->INODE);
	
	char* curPos = fd->data;
	inodeFile* workingDirCont = malloc(inodeSize);
	memcpy(workingDirCont, fd->INODE, inodeSize);
	do {
		int i;
		for (i = 0; i < 6; ++i) {
			if (workingDirCont->sectors[i] != -1) {
				Sector* dataSector = malloc(SD_SECTORSIZE);
				memcpy(dataSector, curPos, SD_SECTORSIZE);
				while ( SD_write(workingDirCont->sectors[i], dataSector) != 0 ) {}
				free(dataSector);
				curPos += SD_SECTORSIZE;
			}
		}
	} while( (workingDirCont = (inodeFile*)getCont((inode*)workingDirCont)) != 0);
}

int countSectorsInFile(inodeFile* inodeF) {
	int value = 0;
	inodeFile* workingDirCont = malloc(inodeSize);
	memcpy(workingDirCont, inodeF, inodeSize);
	do {
		int i;
		for (i = 0; i < 6; ++i) {
			if (workingDirCont->sectors[i] != -1) {
				value++;
			}
		}
	} while( (workingDirCont = (inodeFile*)getCont((inode*)workingDirCont)) != 0);
	return value;
}

void addSector(inodeFile* parent) {
	int i;
	bool added = 0;
	for (i = 0; i < 6; i++) {
		if (parent->sectors[i] == -1) {
			added = 1;
			parent->sectors[i] = useNextFreeSector();
			break;
		}
	}
	if (!added) {
		if (DEBUG) printf("Checking if we need to add a continuing inode (%i) to %i...\n", parent->cont, parent->num);
		if (parent->cont == -1) {
			int contInodeNum = createInode();
			inodeFile* contInode = (inodeFile*)getInode(contInodeNum);
			initFile(contInode, contInodeNum, -1, -1, "");
			parent->cont = contInodeNum;
			saveInode((inode*)contInode);
			free(contInode);
			contInode = (inodeFile*)getInode(contInodeNum);
		}
		if (DEBUG) printf("Adding the child to the continuing inode...\n");
		inode* contInode = getInode(parent->cont);
		addSector((inodeFile*)contInode);
		saveInode(contInode);
		free(contInode);
	}
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
	
	rootInodeNum = createInode();
	cwd = rootInodeNum; // initialize current working directory to root inode
	
	inodeDir* rootInode = (inodeDir*)getInode(cwd);
	initDir(rootInode, rootInodeNum, -1, -1, "");
	saveInode((inode*)rootInode);
	free(rootInode);
	
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

	if (DEBUG) printf("Creating folder %s\n", name);

    bool error = 0;
	bool absolute = name[0] == '/';
	int result = absolute ? rootInodeNum : cwd;
	inodeDir* workingDir = (inodeDir*)getInode(result);
	tokenResult* tokens = parsePath(name);
	int i;
	for (i = 0; i < tokens->numTokens - 1; i++) {
		if (strcmp(tokens->tokens[i], ".") == 0) {
			// do nothing
		} else {
			if (strcmp(tokens->tokens[i], "..") == 0) {
				if (workingDir->parent != -1) {
					int parent = workingDir->parent;
					free(workingDir);
					if (DEBUG) printf("Setting result to %i\n", workingDir->parent);
					workingDir = (inodeDir*)getInode(parent);
					result = workingDir->parent;
				} else {
					error = 1;
				}
			} else {
				inodeDir* workingDirCont = malloc(inodeSize);
				memcpy(workingDirCont, workingDir, inodeSize);
				bool found = 0;
				do {
					int j;
					for (j = 0; j < 6; j++) {
						if (workingDirCont->children[j] != -1) {
							inode* child = getInode(workingDirCont->children[j]);
							
							if (strcmp(child->name, tokens->tokens[i]) == 0) {
								if (!child->isFile) {
									if (DEBUG) printf("Setting result to %i\n", workingDirCont->children[j]);
									result = workingDirCont->children[j];
									free(workingDir);
									workingDir = (inodeDir*)child;
									found = 1;
									break;
								} else {
									error = 1;
									found = 1;
									break;
								}
							}
						}
					}
					if (found) break;
				} while( (workingDirCont = (inodeDir*)getCont((inode*)workingDirCont)) != 0);
				if (!found) {
					error = 1;
				}
			}
		}
	}
	
	if (!error) {
		int newInode = createInode();
		inode* child = getInode(newInode);
		
		if (DEBUG) printf("Creating child %i with a parent of %i\n", newInode, result);
		if (DEBUG) printf("Initializing directory...\n");
		initDir((inodeDir*)child, newInode, result, -1, tokens->tokens[tokens->numTokens - 1]);
		if (DEBUG) printf("Adding child...\n");
		addChild(workingDir, newInode);
		if (DEBUG) printf("Saving child...\n");
		saveInode((inode*)child);
		if (DEBUG) printf("Saving working directory...\n");
		saveInode((inode*)workingDir);
		
		if (DEBUG) printf("Trying to free working directory...\n");
		free(workingDir);
		if (DEBUG) printf("Trying to free child...\n");
		free(child);
		freeToken(tokens);
		return 0;
	} else {
		freeToken(tokens);
		return -1;
	}
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

	if (DEBUG) printf("Cding into %s\n", name);
	bool error = 0;
	bool absolute = name[0] == '/';
	int result = absolute? 0 : cwd;
	inodeDir* workingDir = (inodeDir*)getInode(result);
	tokenResult* tokens = parsePath(name);
	
	if (tokens->numTokens == 0) {
		cwd = rootInodeNum;
		free(workingDir);
		return 0;
	}
	
	int i;
	for (i = 0; i < tokens->numTokens; i++) {
		if (strcmp(tokens->tokens[i], ".") == 0) {
			// do nothing
		} else {
			if (strcmp(tokens->tokens[i], "..") == 0) {
				if (workingDir->parent != -1) {
					int parent = workingDir->parent;
					result = workingDir->parent;
					free(workingDir);
					workingDir = (inodeDir*)getInode(parent);
				} else {
					if (DEBUG) printf("Setting error because we're going up and there's no parent\n");
					error = 1;
				}
			} else {
				inodeDir* workingDirCont = malloc(inodeSize);
				memcpy(workingDirCont, workingDir, inodeSize);
				bool found = 0;
				do {
					int j;
					for (j = 0; j < 6; j++) {
						if (workingDirCont->children[j] != -1) {
							inode* child = getInode(workingDirCont->children[j]);
							if (DEBUG) printf("Found a child %s\n", child->name);
							if (strcmp(child->name, tokens->tokens[i]) == 0) {
								if (!child->isFile) {
									result = workingDirCont->children[j];
									free(workingDir);
									workingDir = (inodeDir*)child;
									found = 1;
									break;
								} else {
									error = 1;
									if (DEBUG) printf("Setting error because we tried to cd into a file\n");
									found = 1;
									break;
								}
							}
						}
					}
					if (found) break;
					if (DEBUG) printf("Cont of current inode is %i\n", workingDirCont->cont);
				} while( (workingDirCont = (inodeDir*)getCont((inode*)workingDirCont)) != 0);
				if (!found) {
					if (DEBUG) printf("Setting error because we didn't find the file. :(\n");
					error = 1;
				}
			}
		}
	}
	
	freeToken(tokens);
	
	if (!error) {
		free(workingDir);
		cwd = result;
		return 0;
	} else {
		return -1;
	}
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
			if (cwi->children[i] != -1) {
				inode* child = getInode(cwi->children[i]);
				fprintf(f, "%s\n", child->name);
				free(child);
			}
		}
	} while( (cwi = (inodeDir*)getCont((inode*)cwi)) != 0);
	free(cwi);
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
	bool error = 0;
	bool absolute = name[0] == '/';
	int result = absolute ? rootInodeNum : cwd;
	inodeDir* workingDir = (inodeDir*)getInode(result);
	tokenResult* tokens = parsePath(name);
	int i;
	
	for (i = 0; i < tokens->numTokens - 1; i++) {
		if (strcmp(tokens->tokens[i], ".") == 0) {
			// do nothing
		} else {
			if (strcmp(tokens->tokens[i], "..") == 0) {
				if (workingDir->parent != -1) {
					int parent = workingDir->parent;
					free(workingDir);
					workingDir = (inodeDir*)getInode(parent);
					result = workingDir->parent;
				} else {
					error = 1;
				}
			} else {
				inodeDir* workingDirCont = malloc(inodeSize);
				memcpy(workingDirCont, workingDir, inodeSize);
				bool found = 0;
				do {
					int j;
					for (j = 0; j < 6; j++) {
						if (workingDirCont->children[j] != -1) {
							inode* child = getInode(workingDirCont->children[j]);
							
							if (strcmp(child->name, tokens->tokens[i]) == 0) {
								if (!child->isFile) {
									result = workingDirCont->children[j];
									free(workingDir);
									workingDir = (inodeDir*)child;
									found = 1;
									break;
								} else {
									error = 1;
									found = 1;
									break;
								}
							}
						}
					}
					if (found) break;
				} while( (workingDirCont = (inodeDir*)getCont((inode*)workingDirCont)) != 0);
				if (!found) {
					error = 1;
				}
			}
		}
	}
	
	if (!error) {
		bool found = 0;
		inode* child;
		inodeDir* workingDirCont = malloc(inodeSize);
		memcpy(workingDirCont, workingDir, inodeSize);
		do {
			for (i = 0; i < 6; ++i) {
				if (workingDirCont->children[i] != -1) {
					child = getInode(workingDirCont->children[i]);
					if (strcmp(child->name, tokens->tokens[tokens->numTokens - 1]) == 0) {
						found = 1;
						break;
					}
					free(child);
				}
			}
			if (found) break;
		} while( (workingDirCont = (inodeDir*)getCont((inode*)workingDirCont)) != 0);
		
		if (!found) {
			int newInode = createInode();
			child = getInode(newInode);
			
			initFile((inodeFile*)child, newInode, result, -1, tokens->tokens[tokens->numTokens - 1]);
			addChild(workingDir, newInode);
			saveInode((inode*)child);
			saveInode((inode*)workingDir);
		}
		
		int fdNum = createFd(child);
		
		free(workingDir);
		
		freeToken(tokens);
		
		return fdNum;
	} else {
		freeToken(tokens);
		return -1;
	}
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
	writeFdToDisk(fileID);
	deleteFd(fileID);
    return 0;
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
    fileDescriptor* fd = findFd(fileID);
	if (fd == NULL) {
		return -1;
	}
	char* curPos = fd->data;
	curPos += fd->curPos;
	int filesize = ((inodeFile*)fd->INODE)->filesize;
	int bytesToRead = length > filesize - fd->curPos ? filesize - fd->curPos : length;
	memcpy(buffer, curPos, bytesToRead);
	fd->curPos += bytesToRead;
    return bytesToRead;
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
	fileDescriptor* fd = findFd(fileID);
	int filesize = ((inodeFile*)fd->INODE)->filesize;
	int sizeDiff = fd->curPos + length - filesize;
	if (DEBUG) printf("Writing to file, changing the size by %i\n", sizeDiff);
	if (sizeDiff > 0) {
		filesize += sizeDiff;
		((inodeFile*)fd->INODE)->filesize += sizeDiff;
	}
	int numSectorsNeeded = ceil( (double)filesize / (double)SD_SECTORSIZE );
	int numSectorsHave = countSectorsInFile((inodeFile*)fd->INODE);
	int i;
	for (i = 0; i < numSectorsNeeded - numSectorsHave; ++i) {
		addSector((inodeFile*)fd->INODE);
	}	
	
	fd->data = realloc(fd->data, filesize);
	memcpy(fd->data + fd->curPos, buffer, length);
	if (DEBUG) printf("The file size after writing is %i\n", ((inodeFile*)fd->INODE)->filesize);
    return length;
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
    fileDescriptor* fd = findFd(fileID);
	if (fd == NULL) {
		return -1;
	}
	if (position >= ((inodeFile*)fd->INODE)->filesize) {
		return -1;
	}
	fd->curPos = position;
    return position;
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
