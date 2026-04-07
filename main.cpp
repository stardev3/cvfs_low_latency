#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<iostream>
#include<chrono>
#include<atomic>
#include<cstdint>

// Some older MinGW (MinGW.org) toolchains ship without working C++11 threads.
// To keep the project "modern C++" and still build on such setups, we use a
// small header-only compatibility shim that provides std::thread/std::mutex.
#if defined(__MINGW32__) && !defined(__MINGW64__)
	#include "mingw-std-threads/mingw.thread.h"
	#include "mingw-std-threads/mingw.mutex.h"
#else
	#include <thread>
	#include <mutex>
#endif

#define MAXINODE 5
#define MAXUFDT 50

#define READ 1
#define WRITE 2

#define MAXFILESIZE 1024

#define REGULAR 1
#define SPECIAL 2

#define START 0
#define CURRENT 1
#define END 2

typedef struct superblock
{
	int TotalInodes;
	int FreeInode;
}SUPERBLOCK, *PSUPERBLOCK;

typedef struct inode
{
	char FileName[50];
	int InodeNumber;
	int FileSize;
	int FileActualSize;
	int FileType;
	char *Buffer;
	int LinkCount;
	int ReferenceCount;
	int permission;   //1   2	3
	// Concurrency: per-inode lock to protect inode metadata + file buffer.
	// We avoid embedding std::mutex directly because this project historically
	// allocates INODE via malloc; using a pointer keeps allocation style intact.
	std::mutex *mtx;
	struct inode *next;
}INODE,*PINODE,**PPINODE;

typedef struct filetable
{
	int readoffset;
	int writeoffset;
	int count;
	int mode;  //1	2	3
	PINODE ptrinode;
}FILETABLE,*PFILETABLE;

typedef struct ufdt
{
	PFILETABLE ptrfiletable;
}UFDT;

UFDT UFDTArr[MAXUFDT];
SUPERBLOCK SUPERBLOCKobj;
PINODE head = NULL;

/////////////////////////////////////////
// ---------------- Concurrency + Perf ----------------
// Locking policy (fine-grained):
// - g_meta_mtx protects global metadata: inode freelist scanning, superblock counters,
//   and UFDT entry allocation/free decisions.
// - g_ufdt_mtx[fd] protects a single UFDT entry + its FILETABLE offsets.
// - inode->mtx protects that file's inode fields and Buffer contents.
//
// Lock order to avoid deadlocks: UFDT(fd) -> inode.
static std::mutex g_meta_mtx;
static std::mutex g_ufdt_mtx[MAXUFDT];
// Perf output itself must be thread-safe; otherwise lines interleave.
static std::mutex g_perf_print_mtx;

// Optional perf modes (opt-in, stderr only):
// - CVFS_PERF=1 enables per-op latency lines in microseconds (default, existing format).
// - CVFS_PERF_NS=1 enables nanosecond latency lines in an alternate format ([PERFNS]).
// - CVFS_PERF_HIST=1 enables a simple histogram dump at process exit (stderr only).
static std::atomic<bool> g_perf_ns_enabled_cached{false};
static std::atomic<bool> g_perf_ns_cached_ready{false};
static std::atomic<bool> g_perf_hist_enabled_cached{false};
static std::atomic<bool> g_perf_hist_cached_ready{false};

static inline bool PerfNsEnabled()
{
	if(!g_perf_ns_cached_ready.load(std::memory_order_acquire))
	{
		const char *v = getenv("CVFS_PERF_NS");
		const bool on = (v != NULL) && (strcmp(v,"1") == 0);
		g_perf_ns_enabled_cached.store(on, std::memory_order_release);
		g_perf_ns_cached_ready.store(true, std::memory_order_release);
	}
	return g_perf_ns_enabled_cached.load(std::memory_order_acquire);
}

static inline bool PerfHistEnabled()
{
	if(!g_perf_hist_cached_ready.load(std::memory_order_acquire))
	{
		const char *v = getenv("CVFS_PERF_HIST");
		const bool on = (v != NULL) && (strcmp(v,"1") == 0);
		g_perf_hist_enabled_cached.store(on, std::memory_order_release);
		g_perf_hist_cached_ready.store(true, std::memory_order_release);
	}
	return g_perf_hist_enabled_cached.load(std::memory_order_acquire);
}

enum PerfOpIndex { PERF_CREATE=0, PERF_DELETE=1, PERF_READ=2, PERF_WRITE=3, PERF_OPS=4 };
static inline int PerfOpToIndex(const char *op)
{
	if(op == NULL) return -1;
	if(strcmp(op,"CreateFile")==0) return PERF_CREATE;
	if(strcmp(op,"DeleteFile")==0) return PERF_DELETE;
	if(strcmp(op,"ReadFile")==0) return PERF_READ;
	if(strcmp(op,"WriteFile")==0) return PERF_WRITE;
	return -1;
}

// Histogram buckets in microseconds: [0], (0..1], (1..2], (2..4] ... up to ~1s.
// Stored as atomic counters to allow updates from multiple threads with low overhead.
static std::atomic<unsigned long long> g_perf_hist[PERF_OPS][21];
static inline int HistBucketUs(long long us)
{
	if(us <= 0) return 0;
	int b = 1;
	long long upper = 1;
	while(b < 20 && us > upper)
	{
		upper <<= 1;
		b++;
	}
	return b;
}

static std::atomic<bool> g_perf_enabled_cached{false};
static std::atomic<bool> g_perf_cached_ready{false};

static inline bool PerfEnabled()
{
	// Perf is opt-in and does not change default CLI output:
	// enable by setting env var CVFS_PERF=1. Logs go to stderr.
	if(!g_perf_cached_ready.load(std::memory_order_acquire))
	{
		const char *v = getenv("CVFS_PERF");
		const bool on = (v != NULL) && (strcmp(v,"1") == 0);
		g_perf_enabled_cached.store(on, std::memory_order_release);
		g_perf_cached_ready.store(true, std::memory_order_release);
	}
	return g_perf_enabled_cached.load(std::memory_order_acquire);
}

struct PerfScope
{
	const char *op;
	std::chrono::high_resolution_clock::time_point start;
	explicit PerfScope(const char *opName) : op(opName)
	{
		if(PerfEnabled() || PerfNsEnabled() || PerfHistEnabled())
			start = std::chrono::high_resolution_clock::now();
	}
	~PerfScope()
	{
		if(!(PerfEnabled() || PerfNsEnabled() || PerfHistEnabled())) return;
		auto end = std::chrono::high_resolution_clock::now();

		const auto dur = end - start;
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();

		if(PerfHistEnabled())
		{
			const int idx = PerfOpToIndex(op);
			if(idx >= 0)
			{
				const int b = HistBucketUs((long long)us);
				g_perf_hist[idx][b].fetch_add(1ULL, std::memory_order_relaxed);
			}
		}

		if(PerfEnabled())
		{
			std::lock_guard<std::mutex> lock(g_perf_print_mtx);
			fprintf(stderr,"[PERF] %s latency: %lld us\n", op, (long long)us);
		}
		if(PerfNsEnabled())
		{
			std::lock_guard<std::mutex> lock(g_perf_print_mtx);
			fprintf(stderr,"[PERFNS] %s latency: %lld ns\n", op, (long long)ns);
		}
	}
};

static void DumpPerfHistogramIfEnabled()
{
	if(!PerfHistEnabled()) return;
	std::lock_guard<std::mutex> lock(g_perf_print_mtx);
	fprintf(stderr,"[PERFHIST] bucket_us: 0,1,2,4,...,2^19\n");
	const char *names[PERF_OPS] = {"CreateFile","DeleteFile","ReadFile","WriteFile"};
	for(int op=0; op<PERF_OPS; op++)
	{
		fprintf(stderr,"[PERFHIST] %s:", names[op]);
		for(int b=0;b<21;b++)
		{
			unsigned long long v = g_perf_hist[op][b].load(std::memory_order_relaxed);
			fprintf(stderr," %llu", v);
		}
		fprintf(stderr,"\n");
	}
}

static inline uint64_t Fnv1a64(const unsigned char *data, size_t n)
{
	// Deterministic, fast checksum for interview-grade verification.
	uint64_t h = 1469598103934665603ULL;
	for(size_t i=0;i<n;i++)
	{
		h ^= (uint64_t)data[i];
		h *= 1099511628211ULL;
	}
	return h;
}

void man(char *name)
{
	if(name == NULL) return;
	
	if(strcmp(name,"create") == 0)
	{
		printf("Description : Used to create new regular file\n");
		printf("Usage : create File_name Permission\n");
	}
	else if(strcmp(name,"read") == 0)
	{
		printf("Description : Used to read data from regular file\n");
		printf("Usage : read File_name No_Of_Bytes_To-Read\n");
	}
	else if(strcmp(name,"write") == 0)
	{
		printf("Description : Used to write into regular file\n");
		printf("Usage : write File_name\n After this enter the data that we want to write\n");
	}
	else if(strcmp(name,"ls") == 0)
	{
		printf("Description : Used to all information of files\n");
		printf("Usage : ls\n");
	}
	else if(strcmp(name,"stat") == 0)
	{
		printf("Description : Used to display information of file\n");
		printf("Usage : stat File_name\n");
	}
	else if(strcmp(name,"fstat") == 0)
	{
		printf("Description : Used to display information of file\n");
		printf("Usage : stat File_Descriptor\n");
	}
	else if(strcmp(name,"truncate") == 0)
	{
		printf("Description : Used to remove data from file\n");
		printf("Usage : truncate File_name\n");
	}
	else if(strcmp(name,"open") == 0)
	{
		printf("Description : Used to Open Existing file\n");
		printf("Usage : open File_name mode\n");
	}
	else if(strcmp(name,"close") == 0)
	{
		printf("Description : Used to close opened file\n");
		printf("Usage : close File_name\n");
	}
	else if(strcmp(name,"closeall") == 0)
	{
		printf("Description : Used to close all opened files\n");
		printf("Usage : closeall\n");
	}
	else if(strcmp(name,"lseek") == 0)
	{
		printf("Description : Used to change file offset\n");
		printf("Usage : lseek File_name ChangeInOffset StartPoint\n");
	}
	else if(strcmp(name,"rm") == 0)
	{
		printf("Description : Used to delete the file\n");
		printf("Usage : rm File_name\n");
	}
	else
	{
		printf("ERROR : No manual entry available.\n");
	}
}

void DisplayHelp()
{
	printf("ls : To List out all files\n");
	printf("clear : To clear console\n");
	printf("create : To create a new regular file\n");
	printf("open : To open the file\n");
	printf("close : To close the file\n");
	printf("closeall : To close all opened files\n");
	printf("read : To read the Contents from file\n");
	printf("write : To write Contents into file\n");
	printf("exit : To Terminate File System\n");
	printf("stat : To Display information of file using name\n");
	printf("fstat : To Display information of file using file Descriptor\n");
	printf("truncate : To remove all data from file\n");
	printf("rm : To Delete the file\n");
}

int GetFDFromName(char *name)
{
	int i = 0;
	
	while(i < MAXUFDT)
	{
		if((UFDTArr[i].ptrfiletable != NULL) && (UFDTArr[i].ptrfiletable->ptrinode->FileType != 0))
			if(strcmp((UFDTArr[i].ptrfiletable->ptrinode->FileName),name) == 0)
				break;
		i++;
	}
	
	if(i == MAXUFDT)   return -1;
	else               return i;
}

PINODE Get_Inode(char *name)
{
	PINODE temp = head;
	int i = 0;
	
	if(name == NULL)
		return NULL;
	
	while(temp != NULL)
	{
		if(strcmp(name,temp->FileName) == 0)
			break;
		temp = temp -> next;
	}
	return temp;
}

void CreateDILB()
{
	int i = 1;
	PINODE newn = NULL;
	PINODE temp = head;
	
	while(i <= MAXINODE)
	{
		newn = (PINODE)malloc(sizeof(INODE));
		if(newn == NULL)
		{
			printf("DILB created successfully\n");
			return;
		}
		
		newn->LinkCount = 0;
		newn->ReferenceCount = 0;
		newn->FileType = 0;
		newn->FileSize = 0;
		
		newn->Buffer = NULL;
		newn->mtx = new std::mutex();
		newn->next = NULL;
		
		newn->InodeNumber = i;
		
		if(temp == NULL)
		{
			head = newn;
			temp = head;
		}
		else
		{
			temp->next = newn;
			temp = temp->next;
		}
		i++;
	}
	printf("DILB created successfully\n");
}

void InitialiseSuperBlock()
{
	int i = 0;
	while(i < MAXUFDT)
	{
		UFDTArr[i].ptrfiletable = NULL;
		i++;
	}
	
	SUPERBLOCKobj.TotalInodes = MAXINODE;
	SUPERBLOCKobj.FreeInode = MAXINODE;
}

int CreateFile(char *name,int permission)
{
	PerfScope perf("CreateFile");
	int i = 3;
	PINODE temp = head;
	
	if((name == NULL) || (permission == 0) || (permission > 3))
		return -1;

	// Concurrency: creating files mutates global metadata (superblock + UFDT + inode list).
	std::lock_guard<std::mutex> meta_lock(g_meta_mtx);

	if(SUPERBLOCKobj.FreeInode == 0)
		return -2;

	if(Get_Inode(name) != NULL)
		return -3;

	while(temp != NULL)
	{
		if(temp->FileType == 0)
			break;
		temp = temp->next;
	}
	if(temp == NULL)
		return -2;

	while(i < MAXUFDT)
	{
		if(UFDTArr[i].ptrfiletable == NULL)
			break;
		i++;
	}
	if(i == MAXUFDT)
		return -2;

	UFDTArr[i].ptrfiletable = (PFILETABLE)malloc(sizeof(FILETABLE));
	if(UFDTArr[i].ptrfiletable == NULL)
		return -4;
	
	UFDTArr[i].ptrfiletable->count = 1;
	UFDTArr[i].ptrfiletable->mode = permission;
	UFDTArr[i].ptrfiletable->readoffset = 0;
	UFDTArr[i].ptrfiletable->writeoffset = 0;
	
	UFDTArr[i].ptrfiletable->ptrinode = temp;

	// Concurrency: take inode lock while initializing inode fields/buffer.
	std::lock_guard<std::mutex> inode_lock(*(temp->mtx));
	strncpy(UFDTArr[i].ptrfiletable->ptrinode->FileName,name,sizeof(UFDTArr[i].ptrfiletable->ptrinode->FileName)-1);
	UFDTArr[i].ptrfiletable->ptrinode->FileName[sizeof(UFDTArr[i].ptrfiletable->ptrinode->FileName)-1] = '\0';
	UFDTArr[i].ptrfiletable->ptrinode->FileType = REGULAR;
	UFDTArr[i].ptrfiletable->ptrinode->ReferenceCount = 1;
	UFDTArr[i].ptrfiletable->ptrinode->LinkCount = 1;
	UFDTArr[i].ptrfiletable->ptrinode->FileSize = MAXFILESIZE;
	UFDTArr[i].ptrfiletable->ptrinode->FileActualSize = 0;
	UFDTArr[i].ptrfiletable->ptrinode->permission = permission;
	UFDTArr[i].ptrfiletable->ptrinode->Buffer = (char *)malloc(MAXFILESIZE);
	if(UFDTArr[i].ptrfiletable->ptrinode->Buffer == NULL)
	{
		// rollback minimal state; preserve return codes for CLI.
		UFDTArr[i].ptrfiletable->ptrinode->FileType = 0;
		UFDTArr[i].ptrfiletable->ptrinode->ReferenceCount = 0;
		UFDTArr[i].ptrfiletable->ptrinode->LinkCount = 0;
		free(UFDTArr[i].ptrfiletable);
		UFDTArr[i].ptrfiletable = NULL;
		return -4;
	}
	memset(UFDTArr[i].ptrfiletable->ptrinode->Buffer,0,MAXFILESIZE);
	(SUPERBLOCKobj.FreeInode)--;

	return i;
}

//	rm_File("Demo.txt")
int rm_File(char *name)
{
	PerfScope perf("DeleteFile");
	int fd = 0;
	
	std::lock_guard<std::mutex> meta_lock(g_meta_mtx);
	fd = GetFDFromName(name);
	if(fd == -1)
		return -1;

	std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
	PINODE inode = UFDTArr[fd].ptrfiletable->ptrinode;
	std::lock_guard<std::mutex> inode_lock(*(inode->mtx));

	(inode->LinkCount)--;
	
	if(inode->LinkCount == 0)
	{
		inode->FileType = 0;
		// Memory handling: free buffer on final unlink.
		if(inode->Buffer != NULL)
		{
			free(inode->Buffer);
			inode->Buffer = NULL;
		}
		free(UFDTArr[fd].ptrfiletable);
	}
	
	UFDTArr[fd].ptrfiletable = NULL;
	(SUPERBLOCKobj.FreeInode)++;
	return 0;
}

int ReadFile(int fd, char *arr, int isize)
{
	PerfScope perf("ReadFile");
	int read_size = 0;
	
	if(fd < 0 || fd >= MAXUFDT) return -1;
	std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
	if(UFDTArr[fd].ptrfiletable == NULL) 		return -1;

	PINODE inode = UFDTArr[fd].ptrfiletable->ptrinode;
	std::lock_guard<std::mutex> inode_lock(*(inode->mtx));
	
	if(UFDTArr[fd].ptrfiletable->mode != READ && UFDTArr[fd].ptrfiletable->mode != READ + WRITE)	return -2;

	if(UFDTArr[fd].ptrfiletable->ptrinode->permission != READ && UFDTArr[fd].ptrfiletable->ptrinode->permission != READ + WRITE)		return -2;
	
	if(UFDTArr[fd].ptrfiletable->readoffset == UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize)		return -3;
	
	if(UFDTArr[fd].ptrfiletable->ptrinode->FileType != REGULAR)		return -4;
	
	read_size = (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) - (UFDTArr[fd].ptrfiletable->readoffset);
	if(read_size < isize)
	{
		strncpy(arr,(UFDTArr[fd].ptrfiletable->ptrinode->Buffer) + (UFDTArr[fd].ptrfiletable->readoffset),read_size);
		
		UFDTArr[fd].ptrfiletable->readoffset = UFDTArr[fd].ptrfiletable->readoffset + read_size;
	}
	else
	{
		strncpy(arr,(UFDTArr[fd].ptrfiletable->ptrinode->Buffer) + (UFDTArr[fd].ptrfiletable->readoffset),isize);
		
		(UFDTArr[fd].ptrfiletable->readoffset) = (UFDTArr[fd].ptrfiletable->readoffset) + isize;
	}
	
	return isize;
}

int WriteFile(int fd, char *arr, int isize)
{
	PerfScope perf("WriteFile");
	if(fd < 0 || fd >= MAXUFDT) return -1;
	std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
	if(UFDTArr[fd].ptrfiletable == NULL) return -1;

	PINODE inode = UFDTArr[fd].ptrfiletable->ptrinode;
	std::lock_guard<std::mutex> inode_lock(*(inode->mtx));

	if(((UFDTArr[fd].ptrfiletable->mode) != WRITE) && ((UFDTArr[fd].ptrfiletable->mode) != READ+WRITE))		return -1;
	
	if(((UFDTArr[fd].ptrfiletable->ptrinode->permission) != WRITE) && ((UFDTArr[fd].ptrfiletable->ptrinode->permission) != READ+WRITE))		return -1;
	
	if((UFDTArr[fd].ptrfiletable->writeoffset) == MAXFILESIZE)   return -2;
	if(((UFDTArr[fd].ptrfiletable->writeoffset) + isize) > MAXFILESIZE) return -2;
	//////////////////////////////////
	if((UFDTArr[fd].ptrfiletable->ptrinode->FileType) != REGULAR)		return -3;
	
	strncpy((UFDTArr[fd].ptrfiletable->ptrinode->Buffer) + (UFDTArr[fd].ptrfiletable->writeoffset),arr,isize);
	
	(UFDTArr[fd].ptrfiletable->writeoffset) = (UFDTArr[fd].ptrfiletable->writeoffset) + isize;
	
	(UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) = (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) + isize;
	
	return isize;
}

int OpenFile(char *name, int mode)
{
	int i = 0;
	PINODE temp = NULL;
	
	if(name == NULL || mode <= 0)
		return -1;

	std::lock_guard<std::mutex> meta_lock(g_meta_mtx);
	temp = Get_Inode(name);
	if(temp == NULL)
		return -2;
	
	if(temp->permission < mode)
		return -3;

	while(i < MAXUFDT)
	{
		if(UFDTArr[i].ptrfiletable == NULL)
			break;
		i++;
	}
	if(i == MAXUFDT) return -1;
	
	UFDTArr[i].ptrfiletable = (PFILETABLE)malloc(sizeof(FILETABLE));
	if(UFDTArr[i].ptrfiletable == NULL)		return -1;
	UFDTArr[i].ptrfiletable->count = 1;
	UFDTArr[i].ptrfiletable->mode = mode;
	if(mode == READ + WRITE)
	{
		UFDTArr[i].ptrfiletable->readoffset = 0;
		UFDTArr[i].ptrfiletable->writeoffset = 0;
	}
	else if(mode == READ)
	{
		UFDTArr[i].ptrfiletable->readoffset = 0;
	}
	else if(mode == WRITE)
	{
		UFDTArr[i].ptrfiletable->writeoffset = 0;
	}
	UFDTArr[i].ptrfiletable->ptrinode = temp;
	{
		std::lock_guard<std::mutex> inode_lock(*(temp->mtx));
		(UFDTArr[i].ptrfiletable->ptrinode->ReferenceCount)++;
	}
	
	return i;
}

void CloseFileByName(int fd)
{
	UFDTArr[fd].ptrfiletable->readoffset = 0;
	UFDTArr[fd].ptrfiletable->writeoffset = 0;
	(UFDTArr[fd].ptrfiletable->ptrinode->ReferenceCount)--;
}

int CloseFileByName(char *name)
{
	int i = 0;
	i = GetFDFromName(name);
	if(i == -1)
		return -1;
	
	UFDTArr[i].ptrfiletable->readoffset = 0;
	UFDTArr[i].ptrfiletable->writeoffset = 0;
	(UFDTArr[i].ptrfiletable->ptrinode->ReferenceCount)--;
	
	return 0;
}

void CloseAllFile()
{
	int i = 0;
	while(i < MAXUFDT)
	{
		if(UFDTArr[i].ptrfiletable != NULL)
		{
			std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[i]);
			UFDTArr[i].ptrfiletable->readoffset = 0;
			UFDTArr[i].ptrfiletable->writeoffset = 0;
			{
				PINODE inode = UFDTArr[i].ptrfiletable->ptrinode;
				std::lock_guard<std::mutex> inode_lock(*(inode->mtx));
				(UFDTArr[i].ptrfiletable->ptrinode->ReferenceCount)--;
			}
			// Preserve original behavior: stops after closing the first found entry.
			break;
		}
		i++;
	}
}

int LseekFile(int fd,int size,int from)
{
	if((fd < 0) || (from > 2))     return -1;
	if(UFDTArr[fd].ptrfiletable == NULL)	return -1;
	std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
	PINODE inode = UFDTArr[fd].ptrfiletable->ptrinode;
	std::lock_guard<std::mutex> inode_lock(*(inode->mtx));
	
	if((UFDTArr[fd].ptrfiletable->mode == READ) || (UFDTArr[fd].ptrfiletable->mode == READ+WRITE))
	{
		if(from == CURRENT)
		{
			if(((UFDTArr[fd].ptrfiletable->readoffset) + size) > UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize)	return -1;
			if(((UFDTArr[fd].ptrfiletable->readoffset) + size) < 0)		return -1;
			(UFDTArr[fd].ptrfiletable->readoffset) = (UFDTArr[fd].ptrfiletable->readoffset) + size;
		}
		else if(from == START)
		{
			if(size > (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize))		return -1;
			if(size < 0) 	return -1;
			(UFDTArr[fd].ptrfiletable->readoffset) = size;
		}
		else if(from == END)
		{
			if((UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) + size > MAXFILESIZE)	return -1;
			if(((UFDTArr[fd].ptrfiletable->readoffset) + size) < 0)		return -1;
			(UFDTArr[fd].ptrfiletable->readoffset) = (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) + size;
		}
	}
	else if(UFDTArr[fd].ptrfiletable->mode == WRITE)
	{
		if(from == CURRENT)
		{
			if(((UFDTArr[fd].ptrfiletable->writeoffset) + size) > MAXFILESIZE)		return -1;
			if(((UFDTArr[fd].ptrfiletable->writeoffset) + size) < 0)		return -1;
			if(((UFDTArr[fd].ptrfiletable->writeoffset) + size) > (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize))
				(UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) = (UFDTArr[fd].ptrfiletable->writeoffset) + size;
			(UFDTArr[fd].ptrfiletable->writeoffset) = (UFDTArr[fd].ptrfiletable->writeoffset) + size;
		}
		else if(from == START)
		{
			if(size > MAXFILESIZE)		return -1;
			if(size < 0)      return -1;
			if(size > (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize))
				(UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) = size;
			(UFDTArr[fd].ptrfiletable->writeoffset) = size;
		}
		else if(from == END)
		{
			if((UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) + size > MAXFILESIZE)	return -1;
			if(((UFDTArr[fd].ptrfiletable->readoffset) + size) < 0)		return -1;
			(UFDTArr[fd].ptrfiletable->readoffset) = (UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize) + size;
		}
	}
	return 0;
}

void ls_file()
{
	int i = 0;
	PINODE temp = head;
	
	if(SUPERBLOCKobj.FreeInode == MAXINODE)
	{
		printf("Error : There are no files\n");
		return;
	}
	
	printf("\nFile Name\t    InodeNumber\t    FileSize\t   Link count\n");
	printf("----------------------------------------------------------------\n");
	while(temp != NULL)
	{
		if(temp->FileType != 0)
		{
			printf("%s\t\t%d\t\t%d\t\t%d\n",temp->FileName,temp->InodeNumber,temp->FileActualSize,temp->LinkCount);
		}
		temp = temp->next;
	}
	printf("----------------------------------------------------------------\n");
}

int fstat_file(int fd)
{
	PINODE temp = head;
	int i = 0;
	
	if(fd < 0)		return -1;
	
	if(UFDTArr[fd].ptrfiletable == NULL)	return -2;
	
	temp = UFDTArr[fd].ptrfiletable->ptrinode;
	
	printf("\n----------------Statistical Information about File-------------\n");
	printf("File Name : %s\n",temp->FileName);
	printf("Inode Number %d\n",temp->InodeNumber);
	printf("File Size : %d\n",temp->FileSize);
	printf("Actual File Size : %d\n",temp->FileActualSize);
	printf("Link Count : %d\n",temp->LinkCount);
	printf("Reference Count : %d\n",temp->ReferenceCount);
	
	if(temp->permission == 1)
		printf("File Permission : Read Only\n");
	else if(temp->permission == 2)
		printf("File Permission : Write\n");
	else if(temp->permission == 3)
		printf("File Permission : Read & Write\n");
	printf("-----------------------------------------------------------------------\n\n");
	
	return 0;
}

int stat_file(char *name)
{
	PINODE temp = head;
	int i = 0;
	
	if(name == NULL)		return -1;
	
	while(temp != NULL)
	{
		if(strcmp(name,temp->FileName) == 0)
			break;
		temp = temp->next;
	}
	
	if(temp == NULL)	return -2;
	
	printf("\n----------------Statistical Information about File-------------\n");
	printf("File Name : %s\n",temp->FileName);
	printf("Inode Number %d\n",temp->InodeNumber);
	printf("File Size : %d\n",temp->FileSize);
	printf("Actual File Size : %d\n",temp->FileActualSize);
	printf("Link Count : %d\n",temp->LinkCount);
	printf("Reference Count : %d\n",temp->ReferenceCount);
	
	if(temp->permission == 1)
		printf("File Permission : Read Only\n");
	else if(temp->permission == 2)
		printf("File Permission : Write\n");
	else if(temp->permission == 3)
		printf("File Permission : Read & Write\n");
	printf("------------------------------------------------------------------\n\n");
	
	return 0;
}

int truncate_File(char *name)
{
	int fd = GetFDFromName(name);
	if(fd == -1)
		return -1;
	std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
	PINODE inode = UFDTArr[fd].ptrfiletable->ptrinode;
	std::lock_guard<std::mutex> inode_lock(*(inode->mtx));

	memset(UFDTArr[fd].ptrfiletable->ptrinode->Buffer,0,1024);
	UFDTArr[fd].ptrfiletable->readoffset = 0;
	UFDTArr[fd].ptrfiletable->writeoffset = 0;
	UFDTArr[fd].ptrfiletable->ptrinode->FileActualSize = 0;
	return 0;
}

// Optional multithreaded stress test entrypoint:
// run as: ./cvfs --mt-test
static int RunMultithreadedTest()
{
	// Does not affect CLI; intended for interview/demo to show thread-safety.
	// Note: uses existing APIs and respects permissions.
	//
	// Defaults: 4 threads total (2 writers + 2 readers), 1000 writes each, 200 reads each.
	// To vary: use `--mt-test <writers> <readers> <writes_per_writer> <reads_per_reader>`.
	const char *fname = "mt_demo";
	(void)rm_File((char*)fname);
	int fd = CreateFile((char*)fname,3);
	if(fd < 0) return fd;

	auto writer = [fd]()
	{
		char buf[64];
		for(int i=0;i<1000;i++)
		{
			snprintf(buf,sizeof(buf),"x%d",i);
			WriteFile(fd,buf,(int)strlen(buf));
		}
	};
	auto reader = [fd]()
	{
		char out[128];
		for(int i=0;i<200;i++)
		{
			memset(out,0,sizeof(out));
			ReadFile(fd,out,10);
		}
	};

	std::thread t1(writer), t2(writer), t3(reader), t4(reader);
	t1.join(); t2.join(); t3.join(); t4.join();
	return 0;
}

int main(int argc, char **argv)
{
	char *ptr = NULL;
	int ret = 0,fd = 0,count = 0;
	char command[4][80], str[80], arr[1024];
	FILE *scriptFp = NULL;
	
	InitialiseSuperBlock();
	CreateDILB();
	atexit(DumpPerfHistogramIfEnabled);

	if(argc > 1 && argv != NULL && argv[1] != NULL &&
	   ((strcmp(argv[1],"--mt-test") == 0) || (strcmp(argv[1],"--mt-test-verify") == 0)))
	{
		// Extended mt-test parameters (all optional):
		// --mt-test <writers> <readers> <writes_per_writer> <reads_per_reader>
		// --mt-test-verify <writers> <readers> <writes_per_writer> <reads_per_reader>
		// `--mt-test-verify` additionally emits a checksum of the final buffer contents.
		const bool verify = (strcmp(argv[1],"--mt-test-verify") == 0);
		int writers = 2, readers = 2, w_iters = 1000, r_iters = 200;
		if(argc > 2) writers = atoi(argv[2]);
		if(argc > 3) readers = atoi(argv[3]);
		if(argc > 4) w_iters = atoi(argv[4]);
		if(argc > 5) r_iters = atoi(argv[5]);
		if(writers <= 0) writers = 1;
		if(readers <= 0) readers = 1;
		if(w_iters < 0) w_iters = 0;
		if(r_iters < 0) r_iters = 0;
		{
			std::lock_guard<std::mutex> lock(g_perf_print_mtx);
			fprintf(stderr,"[MT] writers=%d readers=%d writes_per_writer=%d reads_per_reader=%d\n",
			        writers, readers, w_iters, r_iters);
			if(verify) fprintf(stderr,"[MT] verify=true\n");
		}

		// Re-implement mt-test with configurable thread counts while using the same CVFS APIs.
		const char *fname = "mt_demo";
		(void)rm_File((char*)fname);
		int fd = CreateFile((char*)fname,3);
		if(fd < 0) return fd;

		std::atomic<long long> total_written_bytes{0};
		std::atomic<long long> total_write_calls{0};
		std::atomic<long long> total_write_failures{0};

		auto writer = [fd, w_iters, &total_written_bytes, &total_write_calls, &total_write_failures]()
		{
			char buf[64];
			for(int i=0;i<w_iters;i++)
			{
				snprintf(buf,sizeof(buf),"x%d",i);
				const int sz = (int)strlen(buf);
				const int rc = WriteFile(fd,buf,sz);
				total_write_calls.fetch_add(1, std::memory_order_relaxed);
				if(rc > 0) total_written_bytes.fetch_add(rc, std::memory_order_relaxed);
				else total_write_failures.fetch_add(1, std::memory_order_relaxed);
			}
		};
		auto reader = [fd, r_iters]()
		{
			char out[128];
			for(int i=0;i<r_iters;i++)
			{
				memset(out,0,sizeof(out));
				ReadFile(fd,out,10);
			}
		};

		// Launch threads.
		std::thread *ts = (std::thread*)malloc(sizeof(std::thread) * (writers + readers));
		if(ts == NULL) return -1;
		int idx = 0;
		for(int i=0;i<writers;i++) new (&ts[idx++]) std::thread(writer);
		for(int i=0;i<readers;i++) new (&ts[idx++]) std::thread(reader);
		for(int i=0;i<idx;i++) ts[i].join();
		for(int i=0;i<idx;i++) ts[i].~thread();
		free(ts);

		// Correctness checks (stderr only, mt-test only):
		// Validate basic invariants after concurrent workload completes.
		{
			std::lock_guard<std::mutex> ufdt_lock(g_ufdt_mtx[fd]);
			PINODE inode = (UFDTArr[fd].ptrfiletable != NULL) ? UFDTArr[fd].ptrfiletable->ptrinode : NULL;
			if(inode != NULL)
			{
				std::lock_guard<std::mutex> inode_lock(*(inode->mtx));
				const long long expected = total_written_bytes.load(std::memory_order_relaxed);
				const long long actual = inode->FileActualSize;
				const bool ok_bounds = (actual >= 0 && actual <= MAXFILESIZE);
				const bool ok_size = (actual == expected);
				std::lock_guard<std::mutex> plock(g_perf_print_mtx);
				fprintf(stderr,"[MT][CHECK] write_calls=%lld write_failures=%lld bytes_written=%lld\n",
				        (long long)total_write_calls.load(std::memory_order_relaxed),
				        (long long)total_write_failures.load(std::memory_order_relaxed),
				        (long long)expected);
				fprintf(stderr,"[MT][CHECK] file_actual_size=%lld bounds_ok=%s size_matches=%s\n",
				        (long long)actual,
				        ok_bounds ? "true" : "false",
				        ok_size ? "true" : "false");

				if(verify)
				{
					const size_t n = (actual > 0) ? (size_t)actual : 0;
					uint64_t h = 0;
					if(inode->Buffer != NULL && n > 0)
						h = Fnv1a64((const unsigned char*)inode->Buffer, n);
					fprintf(stderr,"[MT][VERIFY] fnv1a64=0x%016llx bytes=%zu\n",
					        (unsigned long long)h, n);
				}
			}
		}
		return 0;
	}

	// Optional non-interactive mode for testing/perf capture:
	// run: cvfs.exe --script perf_input.txt
	// This does not change the interactive CLI behavior; it only makes it safe
	// to run the same command parser with file-based stdin for automated logs.
	if(argc > 2 && argv != NULL && argv[1] != NULL && argv[2] != NULL && strcmp(argv[1],"--script") == 0)
	{
		scriptFp = fopen(argv[2],"r");
		if(scriptFp == NULL)
		{
			// Keep stdout clean/unchanged for normal CLI; script mode is opt-in.
			fprintf(stderr,"ERROR : Unable to open script file\n");
			return 1;
		}
	}
	
	//Shell
	while(1)
	{
		// In interactive mode the original code used fflush(stdin) (undefined behavior).
		// We preserve it for interactive sessions, but skip it in --script mode to avoid hangs.
		if(scriptFp == NULL) fflush(stdin);
		strcpy(str,"");
		
		printf("\nMarvellous VFS : > ");
		
		if(fgets(str,80,(scriptFp != NULL) ? scriptFp : stdin) == NULL)
		{
			// EOF in script mode (or stdin closed): terminate cleanly.
			break;
		}
		
		count = sscanf(str,"%s %s %s %s",command[0],command[1],command[2],command[3]);
		
		if(count == 1)
		{
			if(strcmp(command[0],"ls") == 0)
			{
				ls_file();
			}
			else if(strcmp(command[0],"closeall") == 0)
			{
				CloseAllFile();
				printf("All files closed successfully\n");
				continue;
			}
			else if(strcmp(command[0],"clear") == 0)
			{
				system("cls");
				continue;
			}
			else if(strcmp(command[0],"help") == 0)
			{
				DisplayHelp();
				continue;
			}
			else if(strcmp(command[0],"exit") == 0)
			{
				printf("Terminating the Marvellous Virtual File System\n");
				break;
			}
			else
			{
				printf("\nERROR : Command not Found!!!\n");
				continue;
			}
		}
		else if(count == 2)
		{
			if(strcmp(command[0],"stat") == 0)
			{
				ret = stat_file(command[1]);
				if(ret == -1)
					printf("ERROR : Incorrect Parameters\n");
				if(ret == -2)
					printf("ERROR : There is no such file\n");
				continue;
			}
			else if(strcmp(command[0],"fstat") == 0)
			{
				ret = fstat_file(atoi(command[1]));
				if(ret == -1)
					printf("ERROR : Incorrect Parameters\n");
				if(ret == -2)
					printf("ERROR : There is no such file\n");
				continue;
			}
			else if(strcmp(command[0],"close") == 0)
			{
				ret = CloseFileByName(command[1]);
				if(ret == -1)
					printf("ERROR : There is no such file\n");
				continue;
			}
			else if(strcmp(command[0],"rm") == 0)
			{
				ret = rm_File(command[1]);
				if(ret == -1)
					printf("ERROR : There is no such file\n");
				continue;
			}
			else if(strcmp(command[0],"man") == 0)
			{
				man(command[1]);
			}
			else if(strcmp(command[0],"write") == 0)
			{
				fd = GetFDFromName(command[1]);
				if(fd == -1)
				{
					printf("ERROR : Incorrect Parameters\n");
					continue;
				}
				printf("Enter the Data : \n");
				if(scriptFp != NULL)
				{
					// Script mode: read the next line from the script file as data.
					if(fgets(arr,sizeof(arr),scriptFp) == NULL)
					{
						printf("ERROR : Incorrect Parameters\n");
						continue;
					}
					// Strip trailing newline(s) for consistent write length.
					size_t n = strlen(arr);
					while(n > 0 && (arr[n-1] == '\n' || arr[n-1] == '\r'))
					{
						arr[n-1] = '\0';
						n--;
					}
				}
				else
				{
					scanf("%[^\n]s",arr);
				}
				
				ret = strlen(arr);
				if(ret == 0)
				{
					printf("ERROR : Incorrect Parameters\n");
					continue;
				}
				ret = WriteFile(fd,arr,ret);
				if(ret == -1)
					printf("ERROR : Permission Denied\n");
				if(ret == -2)
					printf("ERROR : There is no sufficient memory to write\n");
				if(ret == -3)
					printf("ERROR : It is not a regular file\n");
			}
			else if(strcmp(command[0],"truncate") == 0)
			{
				ret = truncate_File(command[1]);
				if(ret == -1)
					printf("ERROR : Incorrect Parameters\n");
			}
			else
			{
				printf("\nERROR : Command not Found!!!\n");
				continue;
			}
		}
		else if(count == 3)
		{
			if(strcmp(command[0],"create") == 0)
			{
				ret = CreateFile(command[1],atoi(command[2]));
				if(ret >= 0)
					printf("File is successfully opened with file Descriptor : %d\n",ret);
				if(ret == -1)
					printf("ERROR : Incorrect Parameters\n");
				if(ret == -2)
					printf("ERROR : There is no Inodes\n");
				if(ret == -3)
					printf("ERROR : File already exists\n");
				if(ret == -4)
					printf("ERROR : Memory Allocation Failure\n");
				continue;
			}
			else if(strcmp(command[0],"open") == 0)
			{
				ret = OpenFile(command[1],atoi(command[2]));
				if(ret >= 0)
					printf("File is successfully opened with file Descriptor : %d\n",ret);
				if(ret == -1)
					printf("ERROR : Incorrect Parameters\n");
				if(ret == -2)
					printf("ERROR : File not Present\n");
				if(ret == -3)
					printf("ERROR : Permission Denied\n");
				continue;
			}
			else if(strcmp(command[0],"read") == 0)
			{
				fd = GetFDFromName(command[1]);
				if(fd == -1)
				{
					printf("ERROR : Incorrect Parameters\n");
					continue;
				}
				ptr = (char *)malloc((size_t)atoi(command[2]) + 1);
				if(ptr == NULL)
				{
					printf("ERROR : Memory Allocation Failure\n");
					continue;
				}
				ret = ReadFile(fd,ptr,atoi(command[2]));
				if(ret == -1)
					printf("ERROR : File Not Existing\n");
				if(ret == -2)
					printf("ERROR : Permission Denied\n");
				if(ret == -3)
					printf("ERROR : Reached at the end of the file\n");
				if(ret == -4)
					printf("ERROR : It is not a regular file\n");
				if(ret == 0)
					printf("ERROR : File Empty\n");
					if(ret > 0)
					{
						write(2,ptr,ret);
					}
					free(ptr);
					continue;	
			}
			else
			{
				printf("\nERROR : Command not Found!!!\n");
				continue;
			}
		}
		else if(count == 4)
		{
			if(strcmp(command[0],"lseek") == 0)
			{
				fd = GetFDFromName(command[1]);
				if(fd == -1)
				{
					printf("ERROR : Incorrect Parameters\n");
					continue;
				}
				ret = LseekFile(fd,atoi(command[2]),atoi(command[3]));
				if(ret == -1)
				{
					printf("ERROR : Unable to perform lseek\n");
				}
			}
			else
			{
				printf("\nERROR : Command not Found!!!\n");
				continue;
			}
		}
		else
		{
			printf("\nERROR : Command not Found!!!\n");
			continue;
		}
	}
	if(scriptFp != NULL) fclose(scriptFp);
	return 0;
}