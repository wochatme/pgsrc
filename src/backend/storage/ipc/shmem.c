/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  create shared memory and initialize shared memory data structures.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES processes share one or more regions of shared memory.
 * The shared memory is created by a postmaster and is inherited
 * by each backend via fork() (or, in some ports, via other OS-specific
 * methods).  The routines in this file are used for allocating and
 * binding to shared memory data structures.
 *
 * NOTES:
 *		(a) There are three kinds of shared memory data structures
 *	available to POSTGRES: fixed-size structures, queues and hash
 *	tables.  Fixed-size structures contain things like global variables
 *	for a module and should never be allocated after the shared memory
 *	initialization phase.  Hash tables have a fixed maximum size, but
 *	their actual size can vary dynamically.  When entries are added
 *	to the table, more space is allocated.  Queues link data structures
 *	that have been allocated either within fixed-size structures or as hash
 *	buckets.  Each shared data structure has a string name to identify
 *	it (assigned in the module that declares it).
 *
 *		(b) During initialization, each module looks for its
 *	shared data structures in a hash table called the "Shmem Index".
 *	If the data structure is not present, the caller can allocate
 *	a new one and initialize it.  If the data structure is present,
 *	the caller "attaches" to the structure by initializing a pointer
 *	in the local address space.
 *		The shmem index has two purposes: first, it gives us
 *	a simple model of how the world looks when a backend process
 *	initializes.  If something is present in the shmem index,
 *	it is initialized.  If it is not, it is uninitialized.  Second,
 *	the shmem index allows us to allocate shared memory on demand
 *	instead of trying to preallocate structures and hard-wire the
 *	sizes and locations in header files.  If you are using a lot
 *	of shared memory in a lot of different places (and changing
 *	things during development), this is important.
 *
 *		(c) In standard Unix-ish environments, individual backends do not
 *	need to re-establish their local pointers into shared memory, because
 *	they inherit correct values of those variables via fork() from the
 *	postmaster.  However, this does not work in the EXEC_BACKEND case.
 *	In ports using EXEC_BACKEND, new backends have to set up their local
 *	pointers using the method described in (b) above.
 *
 *		(d) memory allocation model: shared memory can never be
 *	freed, once allocated.   Each hash table has its own free list,
 *	so hash buckets can be reused when an item is deleted.  However,
 *	if one hash table grows very large and then shrinks, its space
 *	cannot be redistributed to other tables.  We could build a simple
 *	hash bucket garbage collector if need be.  Right now, it seems
 *	unnecessary.
 */

#include "postgres.h"

#include "access/transam.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"

static void *ShmemAllocRaw(Size size, Size *allocated_size);

/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;	/* shared mem segment header */

static void *ShmemBase;			/* start address of shared memory */

static void *ShmemEnd;			/* end+1 address of shared memory */

slock_t    *ShmemLock;			/* spinlock for shared memory and LWLock
								 * allocation */

static HTAB *ShmemIndex = NULL; /* primary index hashtable for shmem */


/*
 *	InitShmemAccess() --- set up basic pointers to shared memory.
 *
 * Note: the argument should be declared "PGShmemHeader *seghdr",
 * but we use void to avoid having to include ipc.h in shmem.h.
 */
void // 就是设置三个全局变量指针，指向共享内存的头尾
InitShmemAccess(void *seghdr)
{
	PGShmemHeader *shmhdr = (PGShmemHeader *) seghdr;

	ShmemSegHdr = shmhdr;
	ShmemBase = (void *) shmhdr;
	ShmemEnd = (char *) ShmemBase + shmhdr->totalsize;
}

/*
 *	InitShmemAllocation() --- set up shared-memory space allocation.
 *
 * This should be called only in the postmaster or a standalone backend.
 */
void // 在整个共享内存分配完成后，初始化一些东西。这个函数只在启动阶段执行一次
InitShmemAllocation(void)
{
	PGShmemHeader *shmhdr = ShmemSegHdr;
	char	   *aligned;

	Assert(shmhdr != NULL);

	/*
	 * Initialize the spinlock used by ShmemAlloc.  We must use
	 * ShmemAllocUnlocked, since obviously ShmemAlloc can't be called yet.
	 */
	// 此时ShmemLock还没有创建，这段代码只在postmaster启动阶段执行
	ShmemLock = (slock_t *) ShmemAllocUnlocked(sizeof(slock_t)); // 分配自旋锁控制未来的共享内存分配

	SpinLockInit(ShmemLock);
	// 从此之后，共享内存的分配都要使用这把锁才能分配
	/*
	 * Allocations after this point should go through ShmemAlloc, which
	 * expects to allocate everything on cache line boundaries.  Make sure the
	 * first allocation begins on a cache line boundary.
	 */
	aligned = (char *)
		(CACHELINEALIGN((((char *) shmhdr) + shmhdr->freeoffset)));
	shmhdr->freeoffset = aligned - (char *) shmhdr;

	/* ShmemIndex can't be set up yet (need LWLocks first) */
	shmhdr->index = NULL;
	ShmemIndex = (HTAB *) NULL; // 主哈希表需要LWLock，所以现在还没有被创建

	/*
	 * Initialize ShmemVariableCache for transaction manager. (This doesn't
	 * really belong here, but not worth moving.)
	 */
	ShmemVariableCache = (VariableCache)
		ShmemAlloc(sizeof(*ShmemVariableCache));
	memset(ShmemVariableCache, 0, sizeof(*ShmemVariableCache));
}

/*
 * ShmemAlloc -- allocate max-aligned chunk from shared memory
 *
 * Throws error if request cannot be satisfied.
 *
 * Assumes ShmemLock and ShmemSegHdr are initialized.
 */
void * // 在共享内存中分配指定大小的内存块,如果分配失败就报错。这个是有锁分配
ShmemAlloc(Size size)
{
	void	   *newSpace;
	Size		allocated_size;

	newSpace = ShmemAllocRaw(size, &allocated_size);
	if (!newSpace)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	return newSpace;
}

/*
 * ShmemAllocNoError -- allocate max-aligned chunk from shared memory
 *
 * As ShmemAlloc, but returns NULL if out of space, rather than erroring.
 */
void * // 不报错的分配共享内存函数，如果分配失败就返回NULL
ShmemAllocNoError(Size size)
{
	Size		allocated_size;

	return ShmemAllocRaw(size, &allocated_size);
}

/*
 * ShmemAllocRaw -- allocate align chunk and return allocated size
 *
 * Also sets *allocated_size to the number of bytes allocated, which will
 * be equal to the number requested plus any padding we choose to add.
 */
static void * // 此时ShmemLock已经准备好了，所以这个分配是串行执行的
ShmemAllocRaw(Size size, Size *allocated_size)
{
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * Ensure all space is adequately aligned.  We used to only MAXALIGN this
	 * space but experience has proved that on modern systems that is not good
	 * enough.  Many parts of the system are very sensitive to critical data
	 * structures getting split across cache line boundaries.  To avoid that,
	 * attempt to align the beginning of the allocation to a cache line
	 * boundary.  The calling code will still need to be careful about how it
	 * uses the allocated space - e.g. by padding each element in an array of
	 * structures out to a power-of-two size - but without this, even that
	 * won't be sufficient.
	 */
	size = CACHELINEALIGN(size);
	*allocated_size = size;

	Assert(ShmemSegHdr != NULL);

	SpinLockAcquire(ShmemLock); // 获取自旋锁，这是串行的关键
	// 下面的代码逻辑和ShmemAllocUnlocked是一样的
	newStart = ShmemSegHdr->freeoffset;

	newFree = newStart + size;
	if (newFree <= ShmemSegHdr->totalsize)
	{
		newSpace = (void *) ((char *) ShmemBase + newStart);
		ShmemSegHdr->freeoffset = newFree;
	}
	else
		newSpace = NULL;

	SpinLockRelease(ShmemLock);

	/* note this assert is okay with newSpace == NULL */
	Assert(newSpace == (void *) CACHELINEALIGN(newSpace));

	return newSpace;
}

/*
 * ShmemAllocUnlocked -- allocate max-aligned chunk from shared memory
 *
 * Allocate space without locking ShmemLock.  This should be used for,
 * and only for, allocations that must happen before ShmemLock is ready.
 *
 * We consider maxalign, rather than cachealign, sufficient here.
 */
void *  // 在共享内存中分配一块内存，此时分配共享内存的自旋锁还没有就绪，所以无锁分配
ShmemAllocUnlocked(Size size)
{
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * Ensure allocated space is adequately aligned.
	 */
	size = MAXALIGN(size);

	Assert(ShmemSegHdr != NULL);

	newStart = ShmemSegHdr->freeoffset; // 被分配内存的起始地址

	newFree = newStart + size; // 挖去一块内存，要调整空闲内存的起始地址
	if (newFree > ShmemSegHdr->totalsize) // 没有足够的空闲空间，就报错退出
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	ShmemSegHdr->freeoffset = newFree; // 更新头部的空闲内存的指针

	newSpace = (void *) ((char *) ShmemBase + newStart);

	Assert(newSpace == (void *) MAXALIGN(newSpace)); // 确保分配的尺寸是按8字节对齐的

	return newSpace;
}

/*
 * ShmemAddrIsValid -- test if an address refers to shared memory
 *
 * Returns true if the pointer points within the shared memory segment.
 */
bool // 就是判断这个地址是否在共享内存的起始终止指针之间
ShmemAddrIsValid(const void *addr)
{
	return (addr >= ShmemBase) && (addr < ShmemEnd);
}

/*
 *	InitShmemIndex() --- set up or attach to shmem index table.
 */
void
InitShmemIndex(void) //创建或者贴到主哈希表上
{
	HASHCTL		info;

	/*
	 * Create the shared memory shmem index.
	 *
	 * Since ShmemInitHash calls ShmemInitStruct, which expects the ShmemIndex
	 * hashtable to exist already, we have a bit of a circularity problem in
	 * initializing the ShmemIndex itself.  The special "ShmemIndex" hash
	 * table name will tell ShmemInitStruct to fake it.
	 */
	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);
	// 创建主哈希表
	ShmemIndex = ShmemInitHash("ShmemIndex", 
							   SHMEM_INDEX_SIZE, SHMEM_INDEX_SIZE,
							   &info,
							   HASH_ELEM | HASH_STRINGS);
}

/*
 * ShmemInitHash -- Create and initialize, or attach to, a
 *		shared memory hash table.
 *
 * We assume caller is doing some kind of synchronization
 * so that two processes don't try to create/initialize the same
 * table at once.  (In practice, all creations are done in the postmaster
 * process; child processes should always be attaching to existing tables.)
 *
 * max_size is the estimated maximum number of hashtable entries.  This is
 * not a hard limit, but the access efficiency will degrade if it is
 * exceeded substantially (since it's used to compute directory size and
 * the hash table buckets will get overfull).
 *
 * init_size is the number of hashtable entries to preallocate.  For a table
 * whose maximum size is certain, this should be equal to max_size; that
 * ensures that no run-time out-of-shared-memory failures can occur.
 *
 * *infoP and hash_flags must specify at least the entry sizes and key
 * comparison semantics (see hash_create()).  Flag bits and values specific
 * to shared-memory hash tables are added here, except that callers may
 * choose to specify HASH_PARTITION and/or HASH_FIXED_SIZE.
 *
 * Note: before Postgres 9.0, this function returned NULL for some failure
 * cases.  Now, it always throws error instead, so callers need not check
 * for NULL.
 */
HTAB *  // 各种共享内存结构都是在postermaster中创建的，子进程只是贴上去使用。这个函数在共享内存中创建一个哈希表
ShmemInitHash(const char *name,		/* table string name for shmem index */
			  long init_size,	/* initial table size */
			  long max_size,	/* max size of the table */
			  HASHCTL *infoP,	/* info about key and bucket size */
			  int hash_flags)	/* info about infoP */
{
	bool		found;
	void	   *location;

	/*
	 * Hash tables allocated in shared memory have a fixed directory; it can't
	 * grow or other backends wouldn't be able to find it. So, make sure we
	 * make it big enough to start with.
	 *
	 * The shared memory allocator must be specified too.
	 */
	infoP->dsize = infoP->max_dsize = hash_select_dirsize(max_size);
	infoP->alloc = ShmemAllocNoError;
	hash_flags |= HASH_SHARED_MEM | HASH_ALLOC | HASH_DIRSIZE;

	/* look it up in the shmem index */
	location = ShmemInitStruct(name,
							   hash_get_shared_size(infoP, hash_flags),
							   &found);

	/*
	 * if it already exists, attach to it rather than allocate and initialize
	 * new space
	 */
	if (found)
		hash_flags |= HASH_ATTACH;

	/* Pass location of hashtable header to hash_create */
	infoP->hctl = (HASHHDR *) location;

	return hash_create(name, init_size, infoP, hash_flags);
}

/*
 * ShmemInitStruct -- Create/attach to a structure in shared memory.
 *
 *		This is called during initialization to find or allocate
 *		a data structure in shared memory.  If no other process
 *		has created the structure, this routine allocates space
 *		for it.  If it exists already, a pointer to the existing
 *		structure is returned.
 *
 *	Returns: pointer to the object.  *foundPtr is set true if the object was
 *		already in the shmem index (hence, already initialized).
 *
 *	Note: before Postgres 9.0, this function returned NULL for some failure
 *	cases.  Now, it always throws error instead, so callers need not check
 *	for NULL.
 */
void * // 创建或者贴上去
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	ShmemIndexEnt *result;
	void	   *structPtr;

	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE); // ShmemIndexLock是控制访问索引表的轻量级锁

	if (!ShmemIndex) // 主哈希表还没有创建
	{
		PGShmemHeader *shmemseghdr = ShmemSegHdr;

		/* Must be trying to create/attach to ShmemIndex itself */
		Assert(strcmp(name, "ShmemIndex") == 0);

		if (IsUnderPostmaster)
		{
			/* Must be initializing a (non-standalone) backend */
			Assert(shmemseghdr->index != NULL);
			structPtr = shmemseghdr->index;
			*foundPtr = true;
		}
		else
		{
			/*
			 * If the shmem index doesn't exist, we are bootstrapping: we must
			 * be trying to init the shmem index itself.
			 *
			 * Notice that the ShmemIndexLock is released before the shmem
			 * index has been initialized.  This should be OK because no other
			 * process can be accessing shared memory yet.
			 */
			Assert(shmemseghdr->index == NULL);
			structPtr = ShmemAlloc(size);  // 此时主哈希表还没有创建，在这里创建主哈希表
			shmemseghdr->index = structPtr; // 头部结构的index指向了主哈希表
			*foundPtr = false;
		}
		LWLockRelease(ShmemIndexLock);
		return structPtr;
	}
	// 此时主哈希表已经创建，现在是在里面搜索条目， HASH_ENTER_NULL表示如果没有找到，就插入一条记录。如果返回NULL，表示内存溢出了(OOM:out of memory)
	/* look it up in the shmem index */
	result = (ShmemIndexEnt *)
		hash_search(ShmemIndex, name, HASH_ENTER_NULL, foundPtr); // 如果没有找到就创建这个条目

	if (!result) // 这种情况是内存溢出OOM，直接报错退出
	{
		LWLockRelease(ShmemIndexLock);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
						name)));
	}

	if (*foundPtr) // 如果找到了，说明别人已经创建了，我直接拿到内存地址就返回了
	{
		/*
		 * Structure is in the shmem index so someone else has allocated it
		 * already.  The size better be the same as the size we are trying to
		 * initialize to, or there is a name conflict (or worse).
		 */
		if (result->size != size)
		{
			LWLockRelease(ShmemIndexLock);
			ereport(ERROR,
					(errmsg("ShmemIndex entry size is wrong for data structure"
							" \"%s\": expected %zu, actual %zu",
							name, size, result->size)));
		}
		structPtr = result->location;
	}
	else
	{   // 第一次创建这个条目，分配具体的内存，返回这个内存地址
		Size		allocated_size;

		/* It isn't in the table yet. allocate and initialize it */
		structPtr = ShmemAllocRaw(size, &allocated_size);
		if (structPtr == NULL)
		{
			/* out of memory; remove the failed ShmemIndex entry */
			hash_search(ShmemIndex, name, HASH_REMOVE, NULL);
			LWLockRelease(ShmemIndexLock);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("not enough shared memory for data structure"
							" \"%s\" (%zu bytes requested)",
							name, size)));
		}
		// result指向了主哈希表中的条目，所以要更新里面的信息
		result->size = size;
		result->allocated_size = allocated_size;
		result->location = structPtr; 
	}

	LWLockRelease(ShmemIndexLock);

	Assert(ShmemAddrIsValid(structPtr));

	Assert(structPtr == (void *) CACHELINEALIGN(structPtr));

	return structPtr;
}


/*
 * Add two Size values, checking for overflow
 */
Size // 把两个值相加，溢出就报错。因为Size类型是64位，所以溢出的可能性不大
add_size(Size s1, Size s2)
{
	Size		result;

	result = s1 + s2;
	/* We are assuming Size is an unsigned type here... */
	if (result < s1 || result < s2) // 溢出了
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/*
 * Multiply two Size values, checking for overflow
 */
Size // 把两个值相乘，检查是否溢出，如果溢出就报错
mul_size(Size s1, Size s2)
{
	Size		result;

	if (s1 == 0 || s2 == 0)
		return 0;
	result = s1 * s2;
	/* We are assuming Size is an unsigned type here... */
	if (result / s2 != s1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested shared memory size overflows size_t")));
	return result;
}

/* SQL SRF showing allocated shared memory */
Datum
pg_get_shmem_allocations(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_SIZES_COLS 4
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Size		named_allocated = 0;
	Datum		values[PG_GET_SHMEM_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_SIZES_COLS];

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* output all allocated entries */
	memset(nulls, 0, sizeof(nulls));
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		values[0] = CStringGetTextDatum(ent->key);
		values[1] = Int64GetDatum((char *) ent->location - (char *) ShmemSegHdr);
		values[2] = Int64GetDatum(ent->size);
		values[3] = Int64GetDatum(ent->allocated_size);
		named_allocated += ent->allocated_size;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	/* output shared memory allocated but not counted via the shmem index */
	values[0] = CStringGetTextDatum("<anonymous>");
	nulls[1] = true;
	values[2] = Int64GetDatum(ShmemSegHdr->freeoffset - named_allocated);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	/* output as-of-yet unused shared memory */
	nulls[0] = true;
	values[1] = Int64GetDatum(ShmemSegHdr->freeoffset);
	nulls[1] = false;
	values[2] = Int64GetDatum(ShmemSegHdr->totalsize - ShmemSegHdr->freeoffset);
	values[3] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	LWLockRelease(ShmemIndexLock);

	return (Datum) 0;
}
