#include <assert.h>
#include <string.h>

#include "kernel_fs.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sys.h"

/*=========================================================

	Inode manipulation

  =========================================================*/


/* --------------------------------------------
	Some convenience methods on Inode objects.
	These methods essentially wrap the FSystem API
	using Inode handles
 ---------------------------------------------- */

/* Return the mount of this inode handle */
inline static FsMount* i_mnt(Inode* inode) { return inode->ino_mnt; }

/* Return MOUNT for inode */
inline static MOUNT i_fsmount(Inode* inode) { return i_mnt(inode)->fsmount; }

/*  Return the mount of this inode handle */
inline static inode_t i_id(Inode* inode) { return inode->ino_id; }

/* Return the file system driver of this inode handle */
inline static FSystem* i_fsys(Inode* inode) {  return i_mnt(inode)->fsys; }


/* Wraps Open */
inline static int i_open(Inode* inode, int flags, void** obj, file_ops** ops)
{
	return i_fsys(inode)->Open(i_fsmount(inode), i_id(inode), flags, obj, ops);
}

/* Wraps Fetch */
inline static int i_fetch(Inode* dir, const pathcomp_t name, inode_t* id, int creat)
{
	return i_fsys(dir)->Fetch(i_fsmount(dir), i_id(dir), name, id, creat);
}


/* Wrap link */
inline static int i_link(Inode* dir, const pathcomp_t name, inode_t inode)
{
	return i_fsys(dir)->Link(i_fsmount(dir), i_id(dir), name, inode);
}


/* Wrap unlink */
inline static int i_unlink(Inode* dir, const pathcomp_t name)
{
	return i_fsys(dir)->Unlink(i_fsmount(dir), i_id(dir), name);
}

/* Wrap Flush */
inline static int i_flush(Inode* inode)
{
	return i_fsys(inode)->Flush(i_fsmount(inode), i_id(inode));
}

/* Wrap Truncate */
inline static int i_truncate(Inode* inode, intptr_t length)
{
	return i_fsys(inode)->Truncate(i_fsmount(inode), i_id(inode), length);
}


/* Return the FSE type of an i-node */
inline static Fse_type i_type(Inode* inode)
{
	struct Stat s;
	int rc = i_fsys(inode)->Status(i_fsmount(inode), i_id(inode), &s, NULL, STAT_TYPE);
	assert(rc==0);
	return s.st_type;
}


/* --------------------------------------------

  Handle management

 ---------------------------------------------- */


/* 
	The inode table is used to map Inode_ref(mnt, id)-> Inode*.
	Using this table, we are sure that there is at most one Inode* object
	for each Inode_ref. 
 */
static rdict inode_table;

/* This type is used as the key of the inode_table */
struct Inode_ref
{
	struct FsMount* mnt;
	inode_t id;
};


/* Equality function for inode_table. The key is assumed to be a pointer to Inode_ref. */
static int inode_table_equal(rlnode* node, rlnode_key key)
{
	/* Get the ino_ref pointer from the key */
	struct Inode_ref* keyiref = key.obj;
	return i_mnt(node->inode)==keyiref->mnt && i_id(node->inode) == keyiref->id;
}


/* Look into the inode_table for existing inode */
Inode* inode_if_pinned(FsMount* mnt, inode_t id)
{
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, inode_table_equal);
	if(node) {
		/* Found it! */
		Inode* ret = node->inode;
		return ret;
	} else {
		return NULL;
	}
}


/*
	Creating a Inode handle.
 */

Inode* pin_inode(FsMount* mnt, inode_t id)
{
	Inode* inode;

	/* Look into the inode_table for existing inode */
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, inode_table_equal);
	if(node) {				/* Found it! */
		inode = node->inode;
		repin_inode(inode);
		return inode;
	}

	/* Get the file system */
	FSystem* fsys = mnt->fsys;

	/* 
		Get a new Inode object and initialize it. This is currently done via malloc, but
		we should really replace this with a pool, for speed.
	 */
	inode = (Inode*) xmalloc(sizeof(Inode));
	rdict_node_init(&inode->inotab_node, inode, hval);

	/* Initialize reference */
	inode->ino_mnt = mnt;
	inode->ino_id = id;
	inode->mounted = NULL;

	/* Pin inode in the file system. If there is an error, clean up and return NULL */
	int rc = fsys->Pin(mnt->fsmount, id);
	if(rc!=0) {
		free(inode);
		set_errcode(rc);
		return NULL;
	}
	
	/* Add to inode table */
	inode->pincount = 1;
	mount_incref(mnt);
	assert(inode_table_equal(&inode->inotab_node, &ino_ref));
	rdict_insert(&inode_table, &inode->inotab_node);

	return inode;
}


void repin_inode(Inode* inode)
{
	inode->pincount ++;
}

int unpin_inode(Inode* inode)
{
	inode->pincount --;
	if(inode->pincount != 0) return 0;

	/* Nobody is pinning the inode, so we release the handle */

	/* Remove it from the inode table */
	rdict_remove(&inode_table, &inode->inotab_node);

	/* Remove reference to mount */
	mount_decref(i_mnt(inode));

	/* Unpin the i-node */
	int rc = i_fsys(inode)->Unpin(i_fsmount(inode), i_id(inode));

	/* Delete the handle and return */
	free(inode);

	if(rc) {
		set_errcode(rc);
		return -1;
	} else 
		return 0;
}



/*=========================================================

	FSys calls

  =========================================================*/


/* File system table */
void register_file_system(FSystem* fsys)
{
	for(unsigned i=0;i<FSYS_MAX; i++) {
		if(file_system_table[i]==NULL) {
			file_system_table[i] = fsys;
			return;
		}
	}
	/* If we reached here, we are in trouble! There are too many file systems. */
	assert(0);
	abort();
}


const char* const * get_filesystems()
{
	static const char* fsnames[FSYS_MAX+1];
	int p=0;

	for(unsigned i=0; i<FSYS_MAX; i++)
		if(file_system_table[i] != NULL)
			fsnames[p++] = file_system_table[i]->name;
	return fsnames;
}


FSystem* get_fsys(const char* fsname)
{
	for(unsigned i=0; i<FSYS_MAX; i++)
		if(file_system_table[i] != NULL 
			&& strcmp(file_system_table[i]->name, fsname)==0)
		{
			return file_system_table[i];
		}
	return NULL;
}

FSystem* file_system_table[FSYS_MAX];


/*=========================================================

	FsMount calls

  =========================================================*/


/* Initialize the root directory */
FsMount mount_table[MOUNT_MAX];

FsMount* mount_acquire()
{
	for(unsigned int i=0; i<MOUNT_MAX; i++) {
		if(mount_table[i].fsys == NULL) {
			mount_table[i].refcount = 0;
			return & mount_table[i];
		}
	}
	return NULL;
}

void mount_incref(FsMount* mnt)
{
	mnt->refcount ++;
}

void mount_decref(FsMount* mnt)
{
	mnt->refcount --;
}



/*=========================================================

	Directory operations

  =========================================================*/

/*
	Get the parent of dir. A new pin is acquired on it.
 */
Inode* dir_parent(Inode* dir)
{
	FsMount* mnt = i_mnt(dir);
	inode_t par_id;

	if(i_fetch(dir, "..", &par_id, 0)!=0) {
		/* Oh dear, we are deleted!! */
		return NULL;
	}

	/* See if we are root */
	if(par_id == i_id(dir)) {
		/* Yes, we are a root in our file system.
		  Take the parent of our mount point */
		if(mnt->mount_point == NULL) {
			/* Aha, we are **THE** root */
			repin_inode(dir);
			return dir;
		} 
		else
			return dir_parent(mnt->mount_point);
	}
	/* We are no root, just pin the parent */
	return pin_inode(mnt, par_id);
}


/* Return true if the name exists in the directory */
int dir_name_exists(Inode* dir, const pathcomp_t name)
{
	if(strcmp(name, ".")==0 || strcmp(name, "..")==0) return 1;
	inode_t id;
	return i_fetch(dir, name, &id, 0) == 0;
}


/* 
	Resolve a member of this directory. A new pin is acquired on it.
	With the creat flag, potentially create a regular file.

	This call knows to traverse mount points. 
	Assume  /mnt/foo is a mount point for mount M and X is the root of M,
	and let Y = dir_lookup(X, "..");
	Then,
	(a) Y corresponds to /mnt (on mount M') and
	(b) dir_lookup(Y, "foo") returns X on mount M 
	  (instead of whatever was /mnt/foo on M')

	A particular idiom is the following:
	// Inode* dir = ...  defined previously

	dir = dir_fetch(dir, ".", 0);
	unpin_inode(dir);

	This ensures that if dir pointed to a mount point, it now points
	to the mounted filesystem.
 */
Inode* dir_fetch(Inode* dir, const pathcomp_t name, int creat)
{
	/* Check the easy case */
	if(strcmp(name, "..")==0) 
		return dir_parent(dir);

	Inode* inode = NULL;

	if(strcmp(name, ".")==0) {
		repin_inode(dir);
		inode = dir;
	} else {
		inode_t id;

		/* Do a lookup */
		if(i_fetch(dir, name, &id, creat) != 0) return NULL;

		/* Pin the fetched i-node */
		inode = pin_inode(i_mnt(dir), id);
		if(inode==NULL) return NULL;
	}
	
	/* Check to see if inode has a mounted directory on it, and
	   if so, take that. */
	if(inode->mounted != NULL) {
		FsMount* mnt = inode->mounted;
		unpin_inode(inode);
		inode = pin_inode(mnt, mnt->root_dir);
		if(inode==NULL) return NULL;
	}

	return inode;
}


/*
	Create a new file system element in a directory. 

	This call can be used to create new members in a directory.
 */
Inode* dir_create(Inode* dir, const pathcomp_t name, Fse_type type, void* data)
{
	FsMount* mnt = i_mnt(dir);
	FSystem* fsys = mnt->fsys;
	inode_t new_id;

	int rc = fsys->Create(mnt->fsmount, i_id(dir), name, type, &new_id, data);
	if(rc != 0) { set_errcode(rc); return NULL; }
	return pin_inode(mnt, new_id);
}



/*=========================================================

	Path manipulation

  =========================================================*/


#define PATHSEP '/'

/* 
	Resolve a pathname.
	
	If last is provided (!= NULL), this call splits the
	pathname into two, as in dirname/last. It returns 
	an Inode to dirname and makes last point to the last
	component of pathname. 
	Note: if the pathname ended in '/' (i.e., "/foo/bar/")
	then *last will be set to NULL.

	If last is NULL, this call returns the Inode corresponding
	to the full pathname.

	In both cases, if there is an error the returned value is
	NULL and set_errcode() has been called with the correct 
	error code.
*/
Inode* lookup_pathname(const char* pathname, const char** last)
{
	int pathlen = strlen(pathname);
	if(pathlen > MAX_PATHNAME) { set_errcode(ENAMETOOLONG); return NULL; }
	if(pathlen==0) { set_errcode(ENOENT); return NULL; }

	const char* base =  pathname;
	const char* cur;
	Inode* inode = NULL;

	/* Local helper function to advance to the next path component */
	int advance() {
		pathcomp_t comp;
		memcpy(comp, base, (cur-base));
		comp[cur-base] = 0;
		Inode* next = dir_fetch(inode, comp, 0);
		unpin_inode(inode);
		inode = next;		
		return next!=NULL; 
	}

	/* Local helper function to check the length of the current path component */
	int length_is_ok() {
		if( (cur-base)>MAX_NAME_LENGTH ) {
			set_errcode(ENAMETOOLONG);
			unpin_inode(inode);
			return 0;
		}
		return 1;
	}

	/* Start with the first character */
	if(*base == '/')  { inode = CURPROC->root_dir; base++; }
	else { inode= CURPROC->cur_dir; }

	/* Obtain a new pin, making sure we are on a top-level inode */
	inode = dir_fetch(inode, ".", 0);

	/* Iterate over all but the last component */
	for(cur = base; *base != '\0'; base=++cur) 
	{
		assert(cur==base);

		/* Get the next component */
		while(*cur != '\0' && *cur != '/') cur++;
		if(cur==base) continue;

		/* cur is at the end, break out to treat last component specially */
		if(*cur=='\0') break;

		/* We have a segment, check it */
		if(! length_is_ok()) return NULL;

		/* ok good size, look it up */
		if(! advance()) return NULL;
	}

	/* (*base) is either 0 or some char */
	assert(*base!='/');  

	/* if last component is empty, pathname ended in '/' */
	assert( (*base != '\0') || (*(base-1)=='/')  );

	/* One last check */
	if(! length_is_ok()) return NULL;

	/* So, at the end either we have a final segment, 
		or *base == '\0' */
	if(last==NULL) {
		if(*base!='\0') {
			/* one last hop */
			if(! advance()) return NULL;
		}	
	} else {
		if(*base!='\0') 
			*last = base;
		else
			*last = NULL;
	}

	return inode;
 }



/*=========================================================


	VFS system calls


  =========================================================*/


/* The cleanup attribute greatly simplifies the implementation of system calls */

void unpin_cleanup(Inode** inoptr) { if(*inoptr) unpin_inode(*inoptr); }
#define AUTO_UNPIN  __attribute__((cleanup(unpin_cleanup)))


Fid_t sys_Open(const char* pathname, int flags)
{
	Fid_t fid;
	FCB* fcb;

	/* Check the flags */
	if( ((flags&OPEN_APPEND) || (flags&OPEN_TRUNC)) && !(flags&OPEN_WRONLY) ) {
		set_errcode(EINVAL);
		return -1;
	}

	/* Try to reserve ids */
	if(! FCB_reserve(1, &fid, &fcb)) {
		return NOFILE;
	}
	/* RESOURCE: Now we have reserved fid/fcb pair */

	/* Take the path */
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);
	if(dir==NULL) {
        FCB_unreserve(1, &fid, &fcb);		
		return NOFILE;
	}
	if(last == NULL) 
		/* The pathname ended in '/', fix it! */
		last = ".";

	/* Try to fetch or create the file */
	Inode* file AUTO_UNPIN = NULL;

	if(flags & OPEN_EXCL) 
		file = dir_create(dir, last, FSE_FILE, NULL);
	else
		file = dir_fetch(dir, last, (flags & OPEN_CREAT) ? 1 : 0);

	/* Have we succeeded? */
	if(file==NULL) {
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;
	}

	/* Possibly truncate the file */
	if(flags & OPEN_TRUNC) {
		int rc = i_truncate(file, 0);
		if(rc) {
			set_errcode(rc);
			FCB_unreserve(1, &fid, &fcb);
			return NOFILE;		
		}
	}

	/* Perform the opening, pass status flags to open */
	int rc = i_open(file, flags & 0xff, &fcb->streamobj, &fcb->streamfunc);
	if(rc!=0) {
		/* Error in open() */
		set_errcode(rc);
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;
	}

	/* Success! */
	return fid;
}



int sys_Stat(const char* pathname, struct Stat* statbuf)
{	
	/* Look it up */
	Inode* inode AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(inode==NULL) return -1;

	int rc = i_fsys(inode)->Status(i_fsmount(inode), i_id(inode), statbuf, NULL, STAT_ALL);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}




int sys_Link(const char* pathname, const char* newpath)
{
	/* Check new path */
	const char* last;
	Inode* newdir AUTO_UNPIN = lookup_pathname(newpath, &last);

	if(newdir == NULL) return -1;
	if(last==NULL || dir_name_exists(newdir, last)) { set_errcode(EEXIST); return -1; }

	Inode* old AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(old==NULL) return -1;

	/* They must be in the same FS */
	if(i_mnt(old) != i_mnt(newdir)) {
		set_errcode(EXDEV);
		return -1;
	}

	int rc = i_link(newdir, last, i_id(old));
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}



int sys_Unlink(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(last==NULL) { set_errcode(EISDIR); return -1; }

	Inode* inode AUTO_UNPIN = dir_fetch(dir, last, 0);

	/* The i-node unlink can treat directories, but we must not let it ... */
	if(i_type(inode)==FSE_DIR) {
		set_errcode(EISDIR);
		return -1;
	}
	
	int rc = i_unlink(dir, last);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}


int sys_MkDir(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(last==NULL || dir_name_exists(dir, last)) { set_errcode(EEXIST); return -1; }
	if(i_type(dir)!=FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	Inode* newdir AUTO_UNPIN = dir_create(dir, last, FSE_DIR, NULL);
	return (newdir == NULL)?-1:0;
}


int sys_RmDir(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);
	if(dir==NULL) { return -1; }
	if(last==NULL) { set_errcode(ENOENT); return -1; }
	if(strcmp(last,".")==0) { set_errcode(EINVAL); return -1; }	
	if(! dir_name_exists(dir,last)) { set_errcode(ENOENT); return -1; }

	int rc = i_unlink(dir, last);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}

int sys_GetCwd(char* buffer, unsigned int size)
{
	Inode* curdir = CURPROC->cur_dir;
	
	char* buf = buffer;
	unsigned int sz = size;

	int bprintf(const char* str) {
		unsigned int len = strlen(str);
		if(len>=sz) {
			set_errcode(ERANGE);
			return -1;
		} else {
			strcpy(buf, str);
			buf+=len;
			sz -= len;
			return 0;
		}
	}

	int print_path_rec(Inode* dir, int level) {
		/* Get your parent */
		Inode* parent AUTO_UNPIN = dir_parent(dir);
		if(parent==NULL) return -1;
		if(parent == dir) {
			/* We are the root */
			return  (level == 0) ? bprintf("/") : 0;
		} else {
			/* We are just a normal dir */
			int rc = print_path_rec(parent, level+1);
			if(rc!=0) return rc;

			pathcomp_t comp;
			i_fsys(dir)->Status(i_fsmount(dir), i_id(dir), NULL, comp, STAT_NAME);
			if(rc!=0) return -1;

			if(bprintf("/")!=0) return-1;
			return bprintf(comp);
		}
	}

	return print_path_rec(curdir, 0);
}

int sys_ChDir(const char* pathname)
{
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(dir==NULL)  return -1;
	if(i_type(dir)!=FSE_DIR) {
		set_errcode(ENOTDIR);
		return -1;
	}

	/* Swap CURPROC->cur_dir  with dir */
	Inode* prevcd = CURPROC->cur_dir;
	CURPROC->cur_dir = dir;
	dir = prevcd;

	return 0;
}

int sys_Mount(Dev_t device, const char* mount_point, const char* fstype, unsigned int paramc, mount_param* paramv)
{
	/* Find the file system */
	FSystem* fsys = get_fsys(fstype);
	if(fsys==NULL) { set_errcode(ENODEV); return -1; }

	/* Get the mount record */
	FsMount* mnt = mount_acquire();
	if(mnt==NULL) return -1;

	/* Resolve mount point */
	Inode* mpoint AUTO_UNPIN = NULL;
	if(mount_point != NULL) {
		mpoint = lookup_pathname(mount_point, NULL);
		if(mpoint==NULL) return -1;
		if(i_type(mpoint) != FSE_DIR) { set_errcode(ENOTDIR); return -1; }
		if(mpoint->pincount != 1) { set_errcode(EBUSY); return -1; }
		assert(mpoint->mounted == NULL);
	} 
	else 
	{
		/* If mpoint is NULL, we must be the root mount */
		if(mnt != mount_table) {
			set_errcode(ENOENT);
			return -1;
		}
	}

	/* TODO: check that the device is not busy */

	/* TODO 2: Check that the device and fstype agree */

	int rc = fsys->Mount(& mnt->fsmount, device, paramc, paramv);
	if(rc==0) {
		if(mpoint) {
			repin_inode(mpoint);
			mpoint->mounted = mnt;
		}

		struct StatFs sfs;
		fsys->StatFs(mnt->fsmount, &sfs);
		mnt->root_dir = sfs.fs_root;

		/* This assignment makes the FsMount object reserved! */
		mnt->fsys = fsys;
		return 0;
	} else {
		set_errcode(rc);
		return -1;
	}
}

int sys_Umount(const char* mount_point)
{
	FsMount* mnt;

	/* Special case: this must be the root mount */
	if(mount_point==NULL) {
		mnt = mount_table;
	} else {
		/* Look up the mount point */
		Inode* mpoint AUTO_UNPIN = lookup_pathname(mount_point, NULL);
		if(mpoint==NULL) return -1;

		if(i_mnt(mpoint)->root_dir != i_id(mpoint)) {
			set_errcode(EINVAL);    /* This is not a mount point */
			return -1;
		}

		mnt = i_mnt(mpoint);
	}

    /* See if we are busy */
    if(mnt->refcount != 0) {
        set_errcode(EBUSY);
        return -1;
    }

    /* Ok, let's unmount */
    int rc = mnt->fsys->Unmount(mnt->fsmount);
    if(rc) { set_errcode(rc); return -1; }

    /* Detach from the filesystem */
    if(mnt->mount_point!=NULL) {
        mnt->mount_point->mounted = NULL;
        rlist_remove(& mnt->submount_node);
    } 

    /* Make mount object unreserved */
    mnt->fsys = NULL;
    return rc;
}


int sys_StatFs(const char* pathname, struct StatFs* statfs)
{
	if(statfs == NULL) return set_errcode(EFAULT), -1;
	Inode* inode AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(inode==NULL) return -1;

	FsMount* mnt = i_mnt(inode);
	FSystem* fsys = mnt->fsys;

	fsys->StatFs(mnt->fsmount, statfs);
	return 0;
}



/*---------------------------------------------
 *
 * Directory listing
 *
 * A dir_list is an object that can be used to
 * make the contents of a directory available
 * to the kernel in a standard format.
 *
 *-------------------------------------------*/

void dir_list_create(dir_list* dlist)
{
	dlist->buffer = NULL;
	dlist->buflen = 0;
	dlist->builder = open_memstream(& dlist->buffer, & dlist->buflen);
}

void dir_list_add(dir_list* dlist, const char* name)
{
	FILE* mfile = dlist->builder;
	unsigned int len = strlen(name);
	assert(len < 256);
    fprintf(mfile, "%02x%s%c", len, name, 0);                  
}

void dir_list_open(dir_list* dlist)
{
	FILE* mfile = dlist->builder;
	fclose(mfile);
	dlist->pos = 0;	
}

int dir_list_read(dir_list* s, char* buf, unsigned int size)
{
	if(size==0) return 0;
	if(s->pos >= s->buflen) return 0;

	size_t remaining_bytes = s->buflen - s->pos;
	size_t txbytes = (remaining_bytes < size) ? remaining_bytes : size ;

	memcpy(buf, s->buffer+s->pos, txbytes);
	s->pos += txbytes;

	return txbytes;
}

intptr_t dir_list_seek(dir_list* s, intptr_t offset, int whence)
{
	intptr_t newpos;
	switch(whence) {
	        case SEEK_SET:
	                newpos = 0; break;
	        case SEEK_CUR: 
	                newpos = s->pos; break;
	        case SEEK_END:
	                newpos = s->buflen; break;
	        default:
	                set_errcode(EINVAL);
	                return -1;
	}

	newpos += offset;
	if(newpos <0 || newpos>= s->buflen) {
	                set_errcode(EINVAL);
	                return -1;              
	}
	s->pos = newpos;
	return newpos;
}

int dir_list_close(dir_list* dlist)
{
	free(dlist->buffer);
	return 0;
}


/*=========================================================


	VFS initialization and finalization


  =========================================================*/


/* Initialization of the file system module */
void initialize_filesys()
{
	/* Init mounts */
	for(unsigned i=0; i<MOUNT_MAX; i++) {
		mount_table[i].refcount = 0;
		rlnode_init(&mount_table[i].submount_node, &mount_table[i]);
		rlnode_init(&mount_table[i].submount_list, NULL);
	}

	/* Init inode_table and root_node */
	rdict_init(&inode_table, MAX_PROC);

	/* FsMount the memfs as the root filesystem */
	int rc = sys_Mount(NO_DEVICE, NULL, "memfs", 0, NULL);
	assert(rc==0);
	if(rc!=0) abort();
}


void finalize_filesys()
{
	/* Unmount root fs */
	int rc = sys_Umount(NULL);
	assert(rc==0);
	if(rc!=0) fprintf(stderr, "Unmounting the root failed, error = %s\n", strerror(rc));

	assert(inode_table.size == 0);
	rdict_destroy(&inode_table);
}



