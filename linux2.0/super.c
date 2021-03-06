/*
 * Super block/filesystem wide operations
 *
 * Peter J. Braam <braam@maths.ox.ac.uk>, 
 * Michael Callahan <callahan@maths.ox.ac.uk> Aug 1996
 *
 */

#define __NO_VERSION__
#include <linux/module.h>

#include <asm/system.h>
#include <asm/segment.h>


#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/locks.h>
#include <linux/string.h>
#include <asm/segment.h>

#include <linux/coda.h>
#include <coda_linux.h>
#include <psdev.h>
#include <super.h>
#include <cnode.h>
#include <upcall.h>
#include <namecache.h>

/* exported variables */
struct super_block *coda_super_block = NULL;
extern struct coda_sb_info coda_super_info;

/* VFS super_block ops */
void print_vattr( struct coda_vattr *attr );
static struct super_block *coda_read_super(struct super_block *, void *, int);
static void coda_read_inode(struct inode *);
int coda_fetch_inode(struct inode *, struct coda_vattr *);
static int  coda_notify_change(struct inode *inode, struct iattr *attr);
static void coda_put_inode(struct inode *);
static void coda_put_super(struct super_block *);
static void coda_statfs(struct super_block *sb, struct statfs *buf, 
		       int bufsiz);


/* helper functions */
void coda_load_creds(struct coda_cred *cred);
extern inline struct vcomm *coda_psdev_vcomm(struct inode *inode);
extern int coda_cnode_make(struct inode **inode, ViceFid *fid, struct super_block *sb);
extern struct cnode *coda_cnode_alloc(void);
extern void coda_cnode_free(struct cnode *);
static int coda_get_psdev(void *, struct inode **);
static void coda_vattr_to_iattr(struct inode *, struct coda_vattr *);
static void coda_iattr_to_vattr(struct iattr *, struct coda_vattr *);

extern int cfsnc_initialized;
extern int coda_debug;
extern int coda_print_entry;

extern struct inode_operations coda_file_inode_operations;
extern struct inode_operations coda_dir_inode_operations;
extern struct inode_operations coda_ioctl_inode_operations;
extern struct inode_operations coda_symlink_inode_operations;
/* exported operations */
struct super_operations coda_super_operations =
{
	coda_read_inode,        /* read_inode */
	coda_notify_change,	/* notify_change */
        NULL,                   /* write_inode */
	coda_put_inode,	        /* put_inode */
	coda_put_super,	        /* put_super */
	NULL,			/* write_super */
	coda_statfs,   		/* statfs */
	NULL			/* remount_fs */
};


static int coda_get_psdev(void *data, struct inode **res_dev)
{
        char **psdev_path = data;
        struct inode *psdev = 0;
        int error = 0;

ENTRY;
        error = namei((char *) *psdev_path, &psdev);
        if (error) {
          CDEBUG(D_SUPER, "namei error %d for %d\n", error, (int) psdev_path);
          goto error;
        }
        

        if (!S_ISCHR(psdev->i_mode)) {
          CDEBUG(D_SUPER, "not a character device\n");
          goto error;
        }
        
        if (MAJOR(psdev->i_rdev) != CODA_PSDEV_MAJOR) {
          CDEBUG(D_SUPER, "device %d not a Coda PSDEV device\n", MAJOR(psdev->i_rdev));
          goto error;
        }

        if (MINOR(psdev->i_rdev) >= MAX_CODADEVS) { 
          CDEBUG(D_SUPER, "minor %d not an allocated Coda PSDEV\n", psdev->i_rdev);
          goto error;
        }

        if (psdev->i_count < 2) {
          CDEBUG(D_SUPER, "device not open (i_count = %d)\n", psdev->i_count);
          goto error;
        }
        
        *res_dev = psdev;

        return 0;
      
EXIT;  
error:
        return 1;
}

static void
coda_read_inode(struct inode *inode)
{
        inode->u.generic_ip = NULL;
}

static struct super_block *
coda_read_super(struct super_block *sb, void *data, int silent)
{
        struct inode *psdev = 0, *root = 0; 
        ViceFid fid;
	kdev_t dev = sb->s_dev;
        int error;

ENTRY;
        MOD_INC_USE_COUNT; 
        if (coda_get_psdev (data, &psdev))
                goto exit;
 
        lock_super(sb);

        coda_sbp(sb) = &coda_super_info;
        coda_super_info.s_psdev = psdev;
        sb->s_blocksize = 1024;	/* smbfs knows what to do? */
        sb->s_blocksize_bits = 10;
        sb->s_magic = CODA_SUPER_MAGIC;
        sb->s_dev = dev;
        sb->s_op = &coda_super_operations;

	/* get root fid from Venus: this needs the root inode */
	error = venus_rootfid(sb, &fid);
	if ( error ) {
	    printk("coda_read_super: coda_get_rootfid failed with %d\n",
		   error);
	    unlock_super(sb);
	    goto exit;
	}	  

	printk("coda_read_super: rootfid is (%ld, %ld, %ld)\n", fid.Volume, fid.Vnode, fid.Unique); 
	
        error = coda_cnode_make(&root, &fid, sb);
        if ( error || !root ) {
	    printk("Failure of coda_cnode_make for root: error %d\n", error);
	    unlock_super(sb);
	    root = NULL;
	    goto exit;
	} 

	printk("coda_read_super: rootinode is %ld dev %d\n", root->i_ino, root->i_dev);
 CDEBUG(D_SUPER, "coda_read_super: root->i_ino: %ld, dev %d\n", root->i_ino,root->i_dev); 
	coda_super_info.mi_rootvp = root;

        sb->s_mounted = root;
	coda_super_block = sb;

	unlock_super(sb);
        return sb;
EXIT;  
exit:

	MOD_DEC_USE_COUNT;
        if (root) {
                iput(root);
                coda_cnode_free(ITOC(root));
        }
        if (psdev)
                iput(psdev);
        sb->s_dev = 0;
        return NULL;
}

int 
coda_fetch_inode (struct inode *inode, struct coda_vattr *attr)
{
        struct cnode *cp;
        int ino, error=0;
ENTRY;
        CDEBUG(D_SUPER, "reading for ino: %ld\n", inode->i_ino);
        ino = inode->i_ino;
        if (!ino)
                coda_panic("coda_fetch_inode: inode called with i_ino = 0\n");

        inode->i_op = NULL;
        inode->i_mode = 0;

        cp = ITOC(inode);
        CHECK_CNODE(cp);
        
        if (cp->c_fid.Volume == 0 &&
            cp->c_fid.Vnode == 0 &&
            cp->c_fid.Unique == 0) {
                /* This is the special root inode created during the sys_mount
                   system call.  We can't do anything here because that call is
                   still executing. */
                inode->i_ino = 1;
                inode->i_op = NULL;
                return 0;
        }
        
        if (IS_CTL_FID( &(cp->c_fid) )) {
                /* This is the special magic control file.  Venus doesn't want
                   to hear a GETATTR about this! */
                inode->i_op = &coda_ioctl_inode_operations;
                return 0;
        }

        if ( ! attr ) {
                printk("coda_fetch_inode: called with NULL vattr, ino %ld\n", inode->i_ino);
                return -1; /* XXXX what to return */
        }

        if (coda_debug & D_SUPER ) print_vattr(attr);
        coda_vattr_to_iattr(inode, attr);

        if (error) {
                /* Since Venus is involved, there is no guarantee of a
                   successful getattr (i.e., a successful read_inode)
                   unlike what happens with Unix filesystem semantics.
                   It's left to the callers of iget to check for this case
                   by consulting the cp->c_invalid field; the caller
                   should then fail its operation and iput the inode
                   immediately to get rid of it, we hope.  Maybe next time
                   getattr will succeed.
                   XXX: Ask Linus about this race condition.  */
                CDEBUG(D_SUPER, "getattr failed");
        }

        if (S_ISREG(inode->i_mode))
                inode->i_op = &coda_file_inode_operations;
        else if (S_ISDIR(inode->i_mode))
                inode->i_op = &coda_dir_inode_operations;
        else if (S_ISLNK(inode->i_mode))
                inode->i_op = &coda_symlink_inode_operations;
        else {
                printk ("coda_read_inode: what kind of inode is this? i_mode = %o\n", inode->i_mode);
                inode->i_op = NULL;
        }
        EXIT;
        return error;
}

static void coda_put_super(struct super_block *sb)
{
        struct coda_sb_info *sb_info;

        ENTRY;

        lock_super(sb);

        sb->s_dev = 0;

        /*  XXXXXXX
	coda_kill(coda_sbp(sb));
        */

	sb_info = coda_sbp(sb);
	iput(sb_info->s_psdev);

	coda_super_block = NULL;
	coda_super_info.mi_rootvp = NULL;

        unlock_super(sb);
        MOD_DEC_USE_COUNT;
EXIT;
}

static void coda_put_inode(struct inode *inode)
{
        struct cnode *cnp;
        struct inode *open_inode;

        ENTRY;
        CDEBUG(D_SUPER, " inode->ino: %ld\n", inode->i_ino);        

	if ( inode->i_ino == CTL_INO ) {
	    clear_inode(inode);
	    return;
	}

        cnp = ITOC(inode);
        open_inode = cnp->c_ovp;

        if ( open_inode ) {
                CDEBUG(D_SUPER, "PUT cached file: ino %ld count %d.\n",  open_inode->i_ino,  open_inode->i_count);
                cnp->c_ovp = NULL;
                iput( open_inode );
        }
        coda_cnode_free(cnp);

        clear_inode(inode);
EXIT;
}

static int  
coda_notify_change(struct inode *inode, struct iattr *iattr)
{
        struct cnode *cnp;
        struct coda_vattr vattr;
        int error;
ENTRY;
        cnp = ITOC(inode);
        CHECK_CNODE(cnp);
	memset(&vattr, 0, sizeof(vattr)); 
	coda_iattr_to_vattr(iattr, &vattr);

DEBUG("XX mtime: %ld\n", inode->i_mtime);
        error = venus_setattr(inode->i_sb, &(cnp->c_fid), &vattr);
        
	if ( error ) {
		CDEBUG(D_SUPER, "venus returned  %d\n", error);
		goto exit;
	} else {
		coda_vattr_to_iattr(inode, &vattr);
		DEBUG("XX mtime: %ld\n", inode->i_mtime);
		cfsnc_zapfid(&(cnp->c_fid));
		DEBUG("XX mtime: %ld\n", inode->i_mtime);
        }
exit: 
        CDEBUG(D_SUPER, " result %d\n", error); 
	EXIT;
        return error;

}
static void coda_vattr_to_iattr(struct inode *inode, struct coda_vattr *attr)
{
        int inode_type;
        /* inode's i_dev, i_flags, i_ino are set by iget 
           XXX: is this all we need ??
           */
        switch (attr->va_type) {
        case C_VNON:
                inode_type  = 0;
                break;
        case C_VREG:
                inode_type = S_IFREG;
                break;
        case C_VDIR:
                inode_type = S_IFDIR;
                break;
        case C_VLNK:
                inode_type = S_IFLNK;
                break;
        default:
                inode_type = 0;
        }
	inode->i_mode |= inode_type;

	if (attr->va_mode != (u_short) -1)
	        inode->i_mode = attr->va_mode | inode_type;
        if (attr->va_uid != -1) 
	        inode->i_uid = (uid_t) attr->va_uid;
        if (attr->va_gid != -1)
	        inode->i_gid = (gid_t) attr->va_gid;
	if (attr->va_nlink != -1)
	        inode->i_nlink = attr->va_nlink;
	if (attr->va_size != -1)
	        inode->i_size = attr->va_size;
	/*  XXX This needs further study */
	/*
        inode->i_blksize = attr->va_blocksize;
	inode->i_blocks = attr->va_size/attr->va_blocksize 
	  + (attr->va_size % attr->va_blocksize ? 1 : 0); 
	  */
	if (attr->va_atime.tv_sec != -1) 
	        inode->i_atime = attr->va_atime.tv_sec;
	if (attr->va_mtime.tv_sec != -1)
	        inode->i_mtime = attr->va_mtime.tv_sec;
        if (attr->va_ctime.tv_sec != -1)
	        inode->i_ctime = attr->va_ctime.tv_sec;
}

/* 
 * BSD sets attributes that need not be modified to -1. 
 * Linux uses the valid field to indicate what should be
 * looked at.  The BSD type field needs to be deduced from linux 
 * mode.
 * So we have to do some translations here.
 */

void coda_iattr_to_vattr(struct iattr *iattr, struct coda_vattr *vattr)
{
        unsigned int valid;

        /* clean out */        
        vattr->va_mode = (umode_t) -1;
        vattr->va_uid = (vuid_t) -1; 
        vattr->va_gid = (vgid_t) -1;
        vattr->va_size = (off_t) -1;
	vattr->va_atime.tv_sec = (time_t) -1;
        vattr->va_mtime.tv_sec  = (time_t) -1;
	vattr->va_ctime.tv_sec  = (time_t) -1;
	vattr->va_atime.tv_nsec =  (time_t) -1;
        vattr->va_mtime.tv_nsec = (time_t) -1;
	vattr->va_ctime.tv_nsec = (time_t) -1;
        vattr->va_type = C_VNON;
	vattr->va_fileid = -1;
	vattr->va_gen = -1;
	vattr->va_bytes = -1;
	vattr->va_nlink = -1;
	vattr->va_blocksize = -1;
	vattr->va_rdev = -1;
        vattr->va_flags = 0;

        /* determine the type */
#if 0
        mode = iattr->ia_mode;
                if ( S_ISDIR(mode) ) {
                vattr->va_type = C_VDIR; 
        } else if ( S_ISREG(mode) ) {
                vattr->va_type = C_VREG;
        } else if ( S_ISLNK(mode) ) {
                vattr->va_type = C_VLNK;
        } else {
                /* don't do others */
                vattr->va_type = C_VNON;
        }
#endif 

        /* set those vattrs that need change */
        valid = iattr->ia_valid;
        if ( valid & ATTR_MODE ) {
                vattr->va_mode = iattr->ia_mode;
	}
        if ( valid & ATTR_UID ) {
                vattr->va_uid = (vuid_t) iattr->ia_uid;
	}
        if ( valid & ATTR_GID ) {
                vattr->va_gid = (vgid_t) iattr->ia_gid;
	}
        if ( valid & ATTR_SIZE ) {
                vattr->va_size = iattr->ia_size;
	}
        if ( valid & ATTR_ATIME ) {
                vattr->va_atime.tv_sec = iattr->ia_atime;
                vattr->va_atime.tv_nsec = 0;
	}
        if ( valid & ATTR_MTIME ) {
                vattr->va_mtime.tv_sec = iattr->ia_mtime;
                vattr->va_mtime.tv_nsec = 0;
	}
        if ( valid & ATTR_CTIME ) {
                vattr->va_ctime.tv_sec = iattr->ia_ctime;
                vattr->va_ctime.tv_nsec = 0;
	}
        
}
  

void
print_vattr( attr )
	struct coda_vattr *attr;
{
    char *typestr;

    switch (attr->va_type) {
    case C_VNON:
	typestr = "C_VNON";
	break;
    case C_VREG:
	typestr = "C_VREG";
	break;
    case C_VDIR:
	typestr = "C_VDIR";
	break;
    case C_VBLK:
	typestr = "C_VBLK";
	break;
    case C_VCHR:
	typestr = "C_VCHR";
	break;
    case C_VLNK:
	typestr = "C_VLNK";
	break;
    case C_VSOCK:
	typestr = "C_VSCK";
	break;
    case C_VFIFO:
	typestr = "C_VFFO";
	break;
    case C_VBAD:
	typestr = "C_VBAD";
	break;
    default:
	typestr = "????";
	break;
    }


    printk("attr: type %s (%o)  mode %o uid %d gid %d rdev %d\n",
	      typestr, (int)attr->va_type, (int)attr->va_mode, (int)attr->va_uid, 
	      (int)attr->va_gid, (int)attr->va_rdev);
    
    printk("      fileid %d nlink %d size %d blocksize %d bytes %d\n",
	      (int)attr->va_fileid, (int)attr->va_nlink, 
	      (int)attr->va_size,
	      (int)attr->va_blocksize,(int)attr->va_bytes);
    printk("      gen %ld flags %ld \n",
	      attr->va_gen, attr->va_flags);
    printk("      atime sec %d nsec %d\n",
	      (int)attr->va_atime.tv_sec, (int)attr->va_atime.tv_nsec);
    printk("      mtime sec %d nsec %d\n",
	      (int)attr->va_mtime.tv_sec, (int)attr->va_mtime.tv_nsec);
    printk("      ctime sec %d nsec %d\n",
	      (int)attr->va_ctime.tv_sec, (int)attr->va_ctime.tv_nsec);
}

/* init_coda: used by filesystems.c to register coda */

struct file_system_type coda_fs_type = {
  coda_read_super, "coda", 0, NULL
};



int init_coda_fs(void)
{
  return register_filesystem(&coda_fs_type);
}



static void coda_statfs(struct super_block *sb, struct statfs *buf, 
		       int bufsiz)
{
	struct statfs tmp;

#define NB_SFS_SIZ 0x895440

	tmp.f_type = CODA_SUPER_MAGIC;
	tmp.f_bsize = 1024;
	tmp.f_blocks = 9000000;
	tmp.f_bfree = 9000000;
	tmp.f_bavail = 9000000 ;
	tmp.f_files = 9000000;
	tmp.f_ffree = 9000000;
	tmp.f_namelen = 0;
	memcpy_tofs(buf, &tmp, bufsiz);
}


