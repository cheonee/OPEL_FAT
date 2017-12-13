/*
 *  linux/fs/vfat/namei.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  Windows95/Windows NT compatible extended MSDOS filesystem
 *    by Gordon Chaffee Copyright (C) 1995.  Send bug reports for the
 *    VFAT filesystem to <chaffee@cs.berkeley.edu>.  Specify
 *    what file operation caused you trouble and if you can duplicate
 *    the problem, send a script that demonstrates it.
 *
 *  Short name translation 1999, 2001 by Wolfram Pienkoss <wp@bszh.de>
 *
 *  Support Multibyte characters and cleanup by
 *				OGAWA Hirofumi <hirofumi@mail.parknet.co.jp>
 */

#define _FAT_
#define __VARIABLE_EXT__
#include<linux/kernel.h>

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/namei.h>
#include "fat.h"


struct PA area_PA[ TOTAL_AREA_CNT ];
//struct PA_unit_t *punit;


char manager_control_sd1[20]; 
char manager_control_sd2[20]; 
struct super_block *g_sb_s1;
struct super_block *g_sb_s2;
unsigned int g_total_cluster_s1[10];
unsigned int g_total_cluster_s2[10];

/*
 * If new entry was created in the parent, it could create the 8.3
 * alias (the shortname of logname).  So, the parent may have the
 * negative-dentry which matches the created 8.3 alias.
 *
 * If it happened, the negative dentry isn't actually negative
 * anymore.  So, drop it.
 */
static int vfat_revalidate_shortname(struct dentry *dentry)
{
	int ret = 1;
	spin_lock(&dentry->d_lock);
	if (dentry->d_time != dentry->d_parent->d_inode->i_version)
		ret = 0;
	spin_unlock(&dentry->d_lock);
	return ret;
}

static int vfat_revalidate(struct dentry *dentry, unsigned int flags)
{
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	/* This is not negative dentry. Always valid. */
	if (dentry->d_inode)
		return 1;
	return vfat_revalidate_shortname(dentry);
}

static int vfat_revalidate_ci(struct dentry *dentry, unsigned int flags)
{
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	/*
	 * This is not negative dentry. Always valid.
	 *
	 * Note, rename() to existing directory entry will have ->d_inode,
	 * and will use existing name which isn't specified name by user.
	 *
	 * We may be able to drop this positive dentry here. But dropping
	 * positive dentry isn't good idea. So it's unsupported like
	 * rename("filename", "FILENAME") for now.
	 */
	if (dentry->d_inode)
		return 1;

	/*
	 * This may be nfsd (or something), anyway, we can't see the
	 * intent of this. So, since this can be for creation, drop it.
	 */
	if (!flags)
		return 0;

	/*
	 * Drop the negative dentry, in order to make sure to use the
	 * case sensitive name which is specified by user if this is
	 * for creation.
	 */
	if (flags & (LOOKUP_CREATE | LOOKUP_RENAME_TARGET))
		return 0;

	return vfat_revalidate_shortname(dentry);
}

/* returns the length of a struct qstr, ignoring trailing dots */
static unsigned int __vfat_striptail_len(unsigned int len, const char *name)
{
	while (len && name[len - 1] == '.')
		len--;
	return len;
}

static unsigned int vfat_striptail_len(const struct qstr *qstr)
{
	return __vfat_striptail_len(qstr->len, qstr->name);
}

/*
 * Compute the hash for the vfat name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The vfat fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int vfat_hash(const struct dentry *dentry, const struct inode *inode,
		struct qstr *qstr)
{
	qstr->hash = full_name_hash(qstr->name, vfat_striptail_len(qstr));
	return 0;
}

/*
 * Compute the hash for the vfat name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The vfat fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int vfat_hashi(const struct dentry *dentry, const struct inode *inode,
		struct qstr *qstr)
{
	struct nls_table *t = MSDOS_SB(dentry->d_sb)->nls_io;
	const unsigned char *name;
	unsigned int len;
	unsigned long hash;

	name = qstr->name;
	len = vfat_striptail_len(qstr);

	hash = init_name_hash();
	while (len--)
		hash = partial_name_hash(nls_tolower(t, *name++), hash);
	qstr->hash = end_name_hash(hash);

	return 0;
}

/*
 * Case insensitive compare of two vfat names.
 */
static int vfat_cmpi(const struct dentry *parent, const struct inode *pinode,
		const struct dentry *dentry, const struct inode *inode,
		unsigned int len, const char *str, const struct qstr *name)
{
	struct nls_table *t = MSDOS_SB(parent->d_sb)->nls_io;
	unsigned int alen, blen;

	/* A filename cannot end in '.' or we treat it like it has none */
	alen = vfat_striptail_len(name);
	blen = __vfat_striptail_len(len, str);
	if (alen == blen) {
		if (nls_strnicmp(t, name->name, str, alen) == 0)
			return 0;
	}
	return 1;
}

/*
 * Case sensitive compare of two vfat names.
 */
static int vfat_cmp(const struct dentry *parent, const struct inode *pinode,
		const struct dentry *dentry, const struct inode *inode,
		unsigned int len, const char *str, const struct qstr *name)
{
	unsigned int alen, blen;

	/* A filename cannot end in '.' or we treat it like it has none */
	alen = vfat_striptail_len(name);
	blen = __vfat_striptail_len(len, str);
	if (alen == blen) {
		if (strncmp(name->name, str, alen) == 0)
			return 0;
	}
	return 1;
}

static const struct dentry_operations vfat_ci_dentry_ops = {
	.d_revalidate	= vfat_revalidate_ci,
	.d_hash		= vfat_hashi,
	.d_compare	= vfat_cmpi,
};

static const struct dentry_operations vfat_dentry_ops = {
	.d_revalidate	= vfat_revalidate,
	.d_hash		= vfat_hash,
	.d_compare	= vfat_cmp,
};

/* Characters that are undesirable in an MS-DOS file name */

static inline wchar_t vfat_bad_char(wchar_t w)
{
	return (w < 0x0020)
	    || (w == '*') || (w == '?') || (w == '<') || (w == '>')
	    || (w == '|') || (w == '"') || (w == ':') || (w == '/')
	    || (w == '\\');
}

static inline wchar_t vfat_replace_char(wchar_t w)
{
	return (w == '[') || (w == ']') || (w == ';') || (w == ',')
	    || (w == '+') || (w == '=');
}

static wchar_t vfat_skip_char(wchar_t w)
{
	return (w == '.') || (w == ' ');
}

static inline int vfat_is_used_badchars(const wchar_t *s, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (vfat_bad_char(s[i]))
			return -EINVAL;

	if (s[i - 1] == ' ') /* last character cannot be space */
		return -EINVAL;

	return 0;
}

static int vfat_find_form(struct inode *dir, unsigned char *name)
{
	struct fat_slot_info sinfo;
	int err = fat_scan(dir, name, &sinfo);
	if (err)
		return -ENOENT;
	brelse(sinfo.bh);
	return 0;
}

/*
 * 1) Valid characters for the 8.3 format alias are any combination of
 * letters, uppercase alphabets, digits, any of the
 * following special characters:
 *     $ % ' ` - @ { } ~ ! # ( ) & _ ^
 * In this case Longfilename is not stored in disk.
 *
 * WinNT's Extension:
 * File name and extension name is contain uppercase/lowercase
 * only. And it is expressed by CASE_LOWER_BASE and CASE_LOWER_EXT.
 *
 * 2) File name is 8.3 format, but it contain the uppercase and
 * lowercase char, muliti bytes char, etc. In this case numtail is not
 * added, but Longfilename is stored.
 *
 * 3) When the one except for the above, or the following special
 * character are contained:
 *        .   [ ] ; , + =
 * numtail is added, and Longfilename must be stored in disk .
 */
struct shortname_info {
	unsigned char lower:1,
		      upper:1,
		      valid:1;
};
#define INIT_SHORTNAME_INFO(x)	do {		\
	(x)->lower = 1;				\
	(x)->upper = 1;				\
	(x)->valid = 1;				\
} while (0)

static inline int to_shortname_char(struct nls_table *nls,
				    unsigned char *buf, int buf_size,
				    wchar_t *src, struct shortname_info *info)
{
	int len;

	if (vfat_skip_char(*src)) {
		info->valid = 0;
		return 0;
	}
	if (vfat_replace_char(*src)) {
		info->valid = 0;
		buf[0] = '_';
		return 1;
	}

	len = nls->uni2char(*src, buf, buf_size);
	if (len <= 0) {
		info->valid = 0;
		buf[0] = '_';
		len = 1;
	} else if (len == 1) {
		unsigned char prev = buf[0];

		if (buf[0] >= 0x7F) {
			info->lower = 0;
			info->upper = 0;
		}

		buf[0] = nls_toupper(nls, buf[0]);
		if (isalpha(buf[0])) {
			if (buf[0] == prev)
				info->lower = 0;
			else
				info->upper = 0;
		}
	} else {
		info->lower = 0;
		info->upper = 0;
	}

	return len;
}

/*
 * Given a valid longname, create a unique shortname.  Make sure the
 * shortname does not exist
 * Returns negative number on error, 0 for a normal
 * return, and 1 for valid shortname
 */
static int vfat_create_shortname(struct inode *dir, struct nls_table *nls,
				 wchar_t *uname, int ulen,
				 unsigned char *name_res, unsigned char *lcase)
{
	struct fat_mount_options *opts = &MSDOS_SB(dir->i_sb)->options;
	wchar_t *ip, *ext_start, *end, *name_start;
	unsigned char base[9], ext[4], buf[5], *p;
	unsigned char charbuf[NLS_MAX_CHARSET_SIZE];
	int chl, chi;
	int sz = 0, extlen, baselen, i, numtail_baselen, numtail2_baselen;
	int is_shortname;
	struct shortname_info base_info, ext_info;

	is_shortname = 1;
	INIT_SHORTNAME_INFO(&base_info);
	INIT_SHORTNAME_INFO(&ext_info);

	/* Now, we need to create a shortname from the long name */
	ext_start = end = &uname[ulen];
	while (--ext_start >= uname) {
		if (*ext_start == 0x002E) {	/* is `.' */
			if (ext_start == end - 1) {
				sz = ulen;
				ext_start = NULL;
			}
			break;
		}
	}

	if (ext_start == uname - 1) {
		sz = ulen;
		ext_start = NULL;
	} else if (ext_start) {
		/*
		 * Names which start with a dot could be just
		 * an extension eg. "...test".  In this case Win95
		 * uses the extension as the name and sets no extension.
		 */
		name_start = &uname[0];
		while (name_start < ext_start) {
			if (!vfat_skip_char(*name_start))
				break;
			name_start++;
		}
		if (name_start != ext_start) {
			sz = ext_start - uname;
			ext_start++;
		} else {
			sz = ulen;
			ext_start = NULL;
		}
	}

	numtail_baselen = 6;
	numtail2_baselen = 2;
	for (baselen = i = 0, p = base, ip = uname; i < sz; i++, ip++) {
		chl = to_shortname_char(nls, charbuf, sizeof(charbuf),
					ip, &base_info);
		if (chl == 0)
			continue;

		if (baselen < 2 && (baselen + chl) > 2)
			numtail2_baselen = baselen;
		if (baselen < 6 && (baselen + chl) > 6)
			numtail_baselen = baselen;
		for (chi = 0; chi < chl; chi++) {
			*p++ = charbuf[chi];
			baselen++;
			if (baselen >= 8)
				break;
		}
		if (baselen >= 8) {
			if ((chi < chl - 1) || (ip + 1) - uname < sz)
				is_shortname = 0;
			break;
		}
	}
	if (baselen == 0) {
		return -EINVAL;
	}

	extlen = 0;
	if (ext_start) {
		for (p = ext, ip = ext_start; extlen < 3 && ip < end; ip++) {
			chl = to_shortname_char(nls, charbuf, sizeof(charbuf),
						ip, &ext_info);
			if (chl == 0)
				continue;

			if ((extlen + chl) > 3) {
				is_shortname = 0;
				break;
			}
			for (chi = 0; chi < chl; chi++) {
				*p++ = charbuf[chi];
				extlen++;
			}
			if (extlen >= 3) {
				if (ip + 1 != end)
					is_shortname = 0;
				break;
			}
		}
	}
	ext[extlen] = '\0';
	base[baselen] = '\0';

	/* Yes, it can happen. ".\xe5" would do it. */
	if (base[0] == DELETED_FLAG)
		base[0] = 0x05;

	/* OK, at this point we know that base is not longer than 8 symbols,
	 * ext is not longer than 3, base is nonempty, both don't contain
	 * any bad symbols (lowercase transformed to uppercase).
	 */

	memset(name_res, ' ', MSDOS_NAME);
	memcpy(name_res, base, baselen);
	memcpy(name_res + 8, ext, extlen);
	*lcase = 0;
	if (is_shortname && base_info.valid && ext_info.valid) {
		if (vfat_find_form(dir, name_res) == 0)
			return -EEXIST;

		if (opts->shortname & VFAT_SFN_CREATE_WIN95) {
			return (base_info.upper && ext_info.upper);
		} else if (opts->shortname & VFAT_SFN_CREATE_WINNT) {
			if ((base_info.upper || base_info.lower) &&
			    (ext_info.upper || ext_info.lower)) {
				if (!base_info.upper && base_info.lower)
					*lcase |= CASE_LOWER_BASE;
				if (!ext_info.upper && ext_info.lower)
					*lcase |= CASE_LOWER_EXT;
				return 1;
			}
			return 0;
		} else {
			BUG();
		}
	}

	if (opts->numtail == 0)
		if (vfat_find_form(dir, name_res) < 0)
			return 0;

	/*
	 * Try to find a unique extension.  This used to
	 * iterate through all possibilities sequentially,
	 * but that gave extremely bad performance.  Windows
	 * only tries a few cases before using random
	 * values for part of the base.
	 */

	if (baselen > 6) {
		baselen = numtail_baselen;
		name_res[7] = ' ';
	}
	name_res[baselen] = '~';
	for (i = 1; i < 10; i++) {
		name_res[baselen + 1] = i + '0';
		if (vfat_find_form(dir, name_res) < 0)
			return 0;
	}

	i = jiffies;
	sz = (jiffies >> 16) & 0x7;
	if (baselen > 2) {
		baselen = numtail2_baselen;
		name_res[7] = ' ';
	}
	name_res[baselen + 4] = '~';
	name_res[baselen + 5] = '1' + sz;
	while (1) {
		snprintf(buf, sizeof(buf), "%04X", i & 0xffff);
		memcpy(&name_res[baselen], buf, 4);
		if (vfat_find_form(dir, name_res) < 0)
			break;
		i -= 11;
	}
	return 0;
}

/* Translate a string, including coded sequences into Unicode */
static int
xlate_to_uni(const unsigned char *name, int len, unsigned char *outname,
	     int *longlen, int *outlen, int escape, int utf8,
	     struct nls_table *nls)
{
	const unsigned char *ip;
	unsigned char nc;
	unsigned char *op;
	unsigned int ec;
	int i, k, fill;
	int charlen;

	if (utf8) {
		*outlen = utf8s_to_utf16s(name, len, UTF16_HOST_ENDIAN,
				(wchar_t *) outname, FAT_LFN_LEN + 2);
		if (*outlen < 0)
			return *outlen;
		else if (*outlen > FAT_LFN_LEN)
			return -ENAMETOOLONG;

		op = &outname[*outlen * sizeof(wchar_t)];
	} else {
		for (i = 0, ip = name, op = outname, *outlen = 0;
			 i < len && *outlen < FAT_LFN_LEN;
			 *outlen += 1) {
			if (escape && (*ip == ':')) {
				if (i > len - 5)
					return -EINVAL;
				ec = 0;
				for (k = 1; k < 5; k++) {
					nc = ip[k];
					ec <<= 4;
					if (nc >= '0' && nc <= '9') {
						ec |= nc - '0';
						continue;
					}
					if (nc >= 'a' && nc <= 'f') {
						ec |= nc - ('a' - 10);
						continue;
					}
					if (nc >= 'A' && nc <= 'F') {
						ec |= nc - ('A' - 10);
						continue;
					}
					return -EINVAL;
				}
				*op++ = ec & 0xFF;
				*op++ = ec >> 8;
				ip += 5;
				i += 5;
			} else {
				charlen = nls->char2uni(ip, len - i,
									(wchar_t *)op);
				if (charlen < 0)
					return -EINVAL;
				ip += charlen;
				i += charlen;
				op += 2;
			}
		}
		if (i < len)
			return -ENAMETOOLONG;
	}

	*longlen = *outlen;
	if (*outlen % 13) {
		*op++ = 0;
		*op++ = 0;
		*outlen += 1;
		if (*outlen % 13) {
			fill = 13 - (*outlen % 13);
			for (i = 0; i < fill; i++) {
				*op++ = 0xff;
				*op++ = 0xff;
			}
			*outlen += fill;
		}
	}

	return 0;
}

static int vfat_build_slots(struct inode *dir, const unsigned char *name,
			    int len, int is_dir, int cluster,
			    struct timespec *ts,
			    struct msdos_dir_slot *slots, int *nr_slots)
{
	struct msdos_sb_info *sbi = MSDOS_SB(dir->i_sb);
	struct fat_mount_options *opts = &sbi->options;
	struct msdos_dir_slot *ps;
	struct msdos_dir_entry *de;
	unsigned char cksum, lcase;
	unsigned char msdos_name[MSDOS_NAME];
	wchar_t *uname;
	__le16 time, date;
	u8 time_cs;
	int err, ulen, usize, i;
	loff_t offset;

	*nr_slots = 0;

	uname = __getname();
	if (!uname)
		return -ENOMEM;

	err = xlate_to_uni(name, len, (unsigned char *)uname, &ulen, &usize,
			   opts->unicode_xlate, opts->utf8, sbi->nls_io);
	if (err)
		goto out_free;

	err = vfat_is_used_badchars(uname, ulen);
	if (err)
		goto out_free;

	err = vfat_create_shortname(dir, sbi->nls_disk, uname, ulen,
				    msdos_name, &lcase);
	if (err < 0)
		goto out_free;
	else if (err == 1) {
		de = (struct msdos_dir_entry *)slots;
		err = 0;
		goto shortname;
	}

	/* build the entry of long file name */
	cksum = fat_checksum(msdos_name);

	*nr_slots = usize / 13;
	for (ps = slots, i = *nr_slots; i > 0; i--, ps++) {
		ps->id = i;
		ps->attr = ATTR_EXT;
		ps->reserved = 0;
		ps->alias_checksum = cksum;
		ps->start = 0;
		offset = (i - 1) * 13;
		fatwchar_to16(ps->name0_4, uname + offset, 5);
		fatwchar_to16(ps->name5_10, uname + offset + 5, 6);
		fatwchar_to16(ps->name11_12, uname + offset + 11, 2);
	}
	slots[0].id |= 0x40;
	de = (struct msdos_dir_entry *)ps;

shortname:
	/* build the entry of 8.3 alias name */
	(*nr_slots)++;
	memcpy(de->name, msdos_name, MSDOS_NAME);
	de->attr = is_dir ? ATTR_DIR : ATTR_ARCH;
	de->lcase = lcase;
	fat_time_unix2fat(sbi, ts, &time, &date, &time_cs);

	//
	time = 123;
	de->time = de->ctime = time;
	de->date = de->cdate = de->adate = date;
	de->ctime_cs = time_cs;
	
	fat_set_start(de, cluster);
	de->size = 0;
out_free:
	__putname(uname);
	return err;
}

static int vfat_add_entry(struct inode *dir, struct qstr *qname, int is_dir,
			  int cluster, struct timespec *ts,
			  struct fat_slot_info *sinfo)
{
	struct msdos_dir_slot *slots;
	unsigned int len;
	int err, nr_slots;

	len = vfat_striptail_len(qname);
	if (len == 0)
		return -ENOENT;

	slots = kmalloc(sizeof(*slots) * MSDOS_SLOTS, GFP_NOFS);
	if (slots == NULL)
		return -ENOMEM;

	err = vfat_build_slots(dir, qname->name, len, is_dir, cluster, ts,
			       slots, &nr_slots);
	if (err)
		goto cleanup;

	err = fat_add_entries(dir, slots, nr_slots, sinfo);
	if (err)
		goto cleanup;

	/* update timestamp */
	dir->i_ctime = dir->i_mtime = dir->i_atime = *ts;

	//TEST_i_atime
//		printk("[cheon] vfat_add_entry  \n");
//		printk("dir->i_mtime.tv_sec : %lu \n", dir->i_mtime.tv_sec );
//		printk("dir->i_mtime.tv_nsec : %ld \n", dir->i_mtime.tv_nsec );



	if (IS_DIRSYNC(dir))
		(void)fat_sync_inode(dir);
	else
		mark_inode_dirty(dir);
cleanup:
	kfree(slots);
	return err;
}

static int vfat_find(struct inode *dir, struct qstr *qname,
		     struct fat_slot_info *sinfo)
{
	unsigned int len = vfat_striptail_len(qname);
	if (len == 0)
		return -ENOENT;
	return fat_search_long(dir, qname->name, len, sinfo);
}

/*
 * (nfsd's) anonymous disconnected dentry?
 * NOTE: !IS_ROOT() is not anonymous (I.e. d_splice_alias() did the job).
 */
static int vfat_d_anon_disconn(struct dentry *dentry)
{
	return IS_ROOT(dentry) && (dentry->d_flags & DCACHE_DISCONNECTED);
}

static struct dentry *vfat_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct fat_slot_info sinfo;
	struct inode *inode;
	struct dentry *alias;
	int err;

//	printk( KERN_ALERT "[cheon] vfat_lookup \n");

	mutex_lock(&MSDOS_SB(sb)->s_lock);

	err = vfat_find(dir, &dentry->d_name, &sinfo);
	if (err) {
		if (err == -ENOENT) {
			inode = NULL;
			goto out;
		}
		goto error;
	}

	inode = fat_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto error;
	}

	alias = d_find_alias(inode);
	if (alias && !vfat_d_anon_disconn(alias)) {
		/*
		 * This inode has non anonymous-DCACHE_DISCONNECTED
		 * dentry. This means, the user did ->lookup() by an
		 * another name (longname vs 8.3 alias of it) in past.
		 *
		 * Switch to new one for reason of locality if possible.
		 */
		BUG_ON(d_unhashed(alias));
		if (!S_ISDIR(inode->i_mode))
			d_move(alias, dentry);
		iput(inode);
		mutex_unlock(&MSDOS_SB(sb)->s_lock);
		return alias;
	} else
		dput(alias);
/////////////////////////////////////////////////////add
#if 1
	if( inode->i_size > 0 && !S_ISDIR( inode->i_mode )  )
	{
	//	printk("[cheon] vfat_lookup \n");

		MSDOS_I( inode )->pre_alloced = ON;	
		MSDOS_I( inode )->pre_count = (inode->i_size) / (sbi->cluster_size);

//		printk("[cheon] vfat_lookup : %d %u  \n", MSDOS_I( inode )->pre_alloced, MSDOS_I( inode )->pre_count );
	}
#endif

out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);
	dentry->d_time = dentry->d_parent->d_inode->i_version;
	dentry = d_splice_alias(inode, dentry);
	if (dentry)
	{
		dentry->d_time = dentry->d_parent->d_inode->i_version;

		//dentry->d_time = 99;
	}

	//printk("[cheon] vfat_lookup : %lu %lu %lu \n", inode->i_ino, inode->i_size, inode->i_blocks );
	//printk("[cheon] vfat_lookup : %lu \n", MSDOS_I( inode )->i_start );
	


	return dentry;

error:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);
	return ERR_PTR(err);
}

static int vfat_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct fat_slot_info sinfo;
	struct timespec ts;
	int err;
	
	//////////
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct PA *pa = NULL;
	//////
//	printk( KERN_ALERT "[cheon] vfat_create \n");

#if 0
	printk("%d\t%d\t%d\t%d\n",

	sbi->parea_PA[ BB_NORMAL ]->active_pa_cnt,
	sbi->parea_PA[ BB_NORMAL_EVENT ]->active_pa_cnt,
	sbi->parea_PA[ BB_PARKING ]->active_pa_cnt,
	sbi->parea_PA[ BB_MANUAL ]->active_pa_cnt );
#endif
	////////////////////////////////////////////////////////
	mutex_lock(&MSDOS_SB(sb)->s_lock);

#if 1
	struct dentry *upper_dentry = NULL;
	

	if( S_ISDIR( dir->i_mode ) )
	{
		upper_dentry = dentry->d_parent;
//		printk("[cheon] dentry %s \n", dentry->d_name.name );
//		printk("[cheon] upper_dentry %s \n", upper_dentry->d_name.name );

		if( upper_dentry->d_name.name != NULL )
		{
			if(strcmp(upper_dentry->d_name.name, DIR_1 ) == 0 ){
				pa = sbi->parea_PA[ BB_NORMAL ];
				if( pa->active_pa_cnt == pa->pa_num ){
					err = -ENOSPC;
					goto out;
				}
			}	
			else if(strcmp(upper_dentry->d_name.name, DIR_2 ) == 0 ){
				pa = sbi->parea_PA[ BB_NORMAL_EVENT ];
				if( pa->active_pa_cnt == pa->pa_num ){
					err = -ENOSPC;
					goto out;
				}
			}	
			else if(strcmp(upper_dentry->d_name.name, DIR_3 ) == 0 ){
				pa = sbi->parea_PA[ BB_PARKING ];
				if( pa->active_pa_cnt == pa->pa_num ){
					err = -ENOSPC;
					goto out;
				}
			}	
			else if(strcmp(upper_dentry->d_name.name, DIR_4 ) == 0 ){
				pa = sbi->parea_PA[ BB_MANUAL ];
				if( pa->active_pa_cnt == pa->pa_num ){
					err = -ENOSPC;
					goto out;
				}
			}	
			else if(strcmp(upper_dentry->d_name.name, DIR_5 ) == 0 ){
	//			pa = sbi->parea_PA[ BB_IMAGE ] 
	//			if( pa->active_pa_cnt == pa->pa_num )		return -ENOSPC;
			}	
			else{
			//	pa = sbi->parea_PA[ BB_ETC ] 
			//	if( pa->active_pa_cnt == pa->pa_num )		return -ENOSPC;
		
			}
		}
	}
#endif


//	ts = CURRENT_TIME_SEC_OPEL;
	ts = CURRENT_TIME_SEC;
	err = vfat_add_entry(dir, &dentry->d_name, 0, 0, &ts, &sinfo);
	
//	printk("[cheon] dentry %s \n", dentry->d_name.name );

	if (err)
	{
		printk("[cheon] vfat_create1 : %d \n", err ); 
		goto out;
	}
	dir->i_version++;

	inode = fat_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		printk("[cheon] vfat_create2 : %d \n", err ); 
		goto out;
	}
	inode->i_version++;

	inode->i_mtime = inode->i_atime = inode->i_ctime = ts;

	/* timestamp is already written, so mark_inode_dirty() is unneeded. */


	dentry->d_time = dentry->d_parent->d_inode->i_version;

	d_instantiate(dentry, inode);
out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);
	return err;
}

static int vfat_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	struct fat_slot_info sinfo;
	int err;

	printk( KERN_ALERT "[cheon] vfat_rmdir \n" ); 

	mutex_lock(&MSDOS_SB(sb)->s_lock);

	err = fat_dir_empty(inode);
	if (err)
		goto out;
	err = vfat_find(dir, &dentry->d_name, &sinfo);
	if (err)
		goto out;

	err = fat_remove_entries(dir, &sinfo);	/* and releases bh */
	if (err)
		goto out;
	drop_nlink(dir);

	clear_nlink(inode);
//	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC_OPEL;
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;

	//cheon
	//TEST_i_atime
//		printk("[cheon] vfat_rmdir \n");
	//	printk("inode->i_mtime.tv_sec : %lu \n", inode->i_mtime.tv_sec );
	//	printk("inode->i_mtime.tv_nsec : %ld \n", inode->i_mtime.tv_nsec );

	fat_detach(inode);
out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);

	return err;
}



static int vfat_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	struct fat_slot_info sinfo;
	int err;

//	printk( KERN_ALERT "[cheon] vfat_unlink \n" );


	mutex_lock(&MSDOS_SB(sb)->s_lock);

	err = vfat_find(dir, &dentry->d_name, &sinfo);
	if (err)
		goto out;

//	printk( KERN_ALERT "[cheon] vfat_unlink \n" ); 
	err = fat_remove_entries(dir, &sinfo);	/* and releases bh */
	if (err)
		goto out;
	clear_nlink(inode);
	//inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC_OPEL;
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;

	//cheon
//		printk("[cheon] vfat_unlink \n");
//		printk("inode->i_mtime.tv_sec : %lu \n", inode->i_mtime.tv_sec );
//		printk("inode->i_mtime.tv_nsec : %ld \n", inode->i_mtime.tv_nsec );


	fat_detach(inode);
out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);

	return err;
}

static int vfat_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct fat_slot_info sinfo;
	struct timespec ts;
	int err, cluster;

//	printk( KERN_ALERT "[cheon] vfat_mkdir \n" );

	mutex_lock(&MSDOS_SB(sb)->s_lock);

	ts = CURRENT_TIME_SEC;
//	ts = CURRENT_TIME_SEC_OPEL;
	cluster = fat_alloc_new_dir(dir, &ts);
	if (cluster < 0) {
		err = cluster;
		goto out;
	}
	err = vfat_add_entry(dir, &dentry->d_name, 1, cluster, &ts, &sinfo);
	if (err)
		goto out_free;
	dir->i_version++;
	inc_nlink(dir);

	inode = fat_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		/* the directory was completed, just return a error */
		goto out;
	}
	inode->i_version++;
	set_nlink(inode, 2);
	inode->i_mtime = inode->i_atime = inode->i_ctime = ts;
	/* timestamp is already written, so mark_inode_dirty() is unneeded. */

		
//		inode->i_mtime.tv_sec  = 1400000000;
//		inode->i_mtime.tv_nsec = 1; 

//		printk("[cheon] vfat_mkdir \n");
//		printk("inode->i_mtime.tv_sec : %lu \n",  inode->i_mtime.tv_sec );
//		printk("inode->i_mtime.tv_nsec : %ld \n", inode->i_mtime.tv_nsec );


	dentry->d_time = dentry->d_parent->d_inode->i_version;
//	dentry->d_time = 99;
	d_instantiate(dentry, inode);

	mutex_unlock(&MSDOS_SB(sb)->s_lock);
	return 0;

out_free:
//	printk( KERN_ALERT "[cheon] vfat_mkdir, fat_free_clusters \n");
	fat_free_clusters(dir, cluster);
out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);
	return err;
}

static int vfat_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry)
{
	struct buffer_head *dotdot_bh;
	struct msdos_dir_entry *dotdot_de;
	struct inode *old_inode, *new_inode;
	struct fat_slot_info old_sinfo, sinfo;
	struct timespec ts;
	loff_t new_i_pos;
	int err, is_dir, update_dotdot, corrupt = 0;
	struct super_block *sb = old_dir->i_sb;

//	printk( KERN_ALERT "[cheon] vfat_rename \n");

	old_sinfo.bh = sinfo.bh = dotdot_bh = NULL;
	old_inode = old_dentry->d_inode;
	new_inode = new_dentry->d_inode;
	mutex_lock(&MSDOS_SB(sb)->s_lock);
	err = vfat_find(old_dir, &old_dentry->d_name, &old_sinfo);
	if (err)
		goto out;

	is_dir = S_ISDIR(old_inode->i_mode);
	update_dotdot = (is_dir && old_dir != new_dir);
	if (update_dotdot) {
		if (fat_get_dotdot_entry(old_inode, &dotdot_bh, &dotdot_de)) {
			err = -EIO;
			goto out;
		}
	}

//	ts = CURRENT_TIME_SEC_OPEL;
	ts = CURRENT_TIME_SEC;
	if (new_inode) {
		if (is_dir) {
			err = fat_dir_empty(new_inode);
			if (err)
				goto out;
		}
		new_i_pos = MSDOS_I(new_inode)->i_pos;
		fat_detach(new_inode);
	} else {
		err = vfat_add_entry(new_dir, &new_dentry->d_name, is_dir, 0,
				     &ts, &sinfo);
		if (err)
			goto out;
		new_i_pos = sinfo.i_pos;
	}
	new_dir->i_version++;

	fat_detach(old_inode);
	fat_attach(old_inode, new_i_pos);
	if (IS_DIRSYNC(new_dir)) {
		err = fat_sync_inode(old_inode);
		if (err)
			goto error_inode;
	} else
		mark_inode_dirty(old_inode);

	if (update_dotdot) {
		fat_set_start(dotdot_de, MSDOS_I(new_dir)->i_logstart);
		mark_buffer_dirty_inode(dotdot_bh, old_inode);
		if (IS_DIRSYNC(new_dir)) {
			err = sync_dirty_buffer(dotdot_bh);
			if (err)
				goto error_dotdot;
		}
		drop_nlink(old_dir);
		if (!new_inode)
 			inc_nlink(new_dir);
	}

	err = fat_remove_entries(old_dir, &old_sinfo);	/* and releases bh */
	old_sinfo.bh = NULL;
	if (err)
		goto error_dotdot;
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = ts;
	if (IS_DIRSYNC(old_dir))
		(void)fat_sync_inode(old_dir);
	else
		mark_inode_dirty(old_dir);

	if (new_inode) {
		drop_nlink(new_inode);
		if (is_dir)
			drop_nlink(new_inode);
		new_inode->i_ctime = ts;
	}
out:
	brelse(sinfo.bh);
	brelse(dotdot_bh);
	brelse(old_sinfo.bh);
	mutex_unlock(&MSDOS_SB(sb)->s_lock);

	return err;

error_dotdot:
	/* data cluster is shared, serious corruption */
	corrupt = 1;

	if (update_dotdot) {
		fat_set_start(dotdot_de, MSDOS_I(old_dir)->i_logstart);
		mark_buffer_dirty_inode(dotdot_bh, old_inode);
		corrupt |= sync_dirty_buffer(dotdot_bh);
	}
error_inode:
	fat_detach(old_inode);
	fat_attach(old_inode, old_sinfo.i_pos);
	if (new_inode) {
		fat_attach(new_inode, new_i_pos);
		if (corrupt)
			corrupt |= fat_sync_inode(new_inode);
	} else {
		/*
		 * If new entry was not sharing the data cluster, it
		 * shouldn't be serious corruption.
		 */
//		printk( KERN_ALERT "[cheon] vfat_rename error_inode \n");
		int err2 = fat_remove_entries(new_dir, &sinfo);
		if (corrupt)
			corrupt |= err2;
		sinfo.bh = NULL;
	}
	if (corrupt < 0) {
		fat_fs_error(new_dir->i_sb,
			     "%s: Filesystem corrupted (i_pos %lld)",
			     __func__, sinfo.i_pos);
	}
	goto out;
}

static const struct inode_operations vfat_dir_inode_operations = {
	.create		= vfat_create,
	.lookup		= vfat_lookup,
	.unlink		= vfat_unlink,
	.mkdir		= vfat_mkdir,
	.rmdir		= vfat_rmdir,
	.rename		= vfat_rename,
	.setattr	= fat_setattr,
	.getattr	= fat_getattr,
};

static void setup(struct super_block *sb)
{
	MSDOS_SB(sb)->dir_ops = &vfat_dir_inode_operations;
	if (MSDOS_SB(sb)->options.name_check != 's')
		sb->s_d_op = &vfat_ci_dentry_ops;
	else
		sb->s_d_op = &vfat_dentry_ops;
}

static inline unsigned int align_check( struct msdos_sb_info *sbi, unsigned int next, int area )
{	
	if( next % CLUSTER_IN_PAGE == 0 );
	else
		next = next + (CLUSTER_IN_PAGE  - (next % CLUSTER_IN_PAGE )); 

	if( (sbi->fat_start % 8) != 0 ) //FAT strat point not match with align
	{
		int adjust = sbi->fat_start % BLOCK_IN_PAGE ;
		next = next - (adjust * CLUSTER_IN_BLOCK );
		//prev = next - 1;
		
		if(next < sbi->bx_start_cluster[area])	{
			next = next + CLUSTER_IN_PAGE ;
		//	prev = next -1;
		}
	}

	return next;
}

static void setting_start_end_in_memory( struct super_block *sb )
{
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct PA_unit_t *punit;
	unsigned int start = 0, end = 0, num_pre_alloc= 0;
	int cnt=0, i=0;

	for( i = 1 ; i < (TOTAL_AREA_CNT-1) ; i++ )
//	i = 1;
	{
		start = align_check( sbi, sbi->bx_prev_free[i]+1, i );	
		punit = area_PA[ i ].pa_unit;

		num_pre_alloc = ( sbi->bx_pre_size[ i ] * 1024 ) / ( sbi->cluster_size / 1024 );

//		printk("[cheon] ==area change==\n");

		while(1)
		{
			punit[cnt].start = start;
			punit[cnt].end = start + num_pre_alloc - 1;

			start += num_pre_alloc;	
			
			punit[cnt].flag = FREE; //일단 전부 FREE
			if( i == 1 )	
			printk("[cheon] start, end : %u, %u \n",  punit[cnt].start, punit[cnt].end );

			cnt++;
			if( cnt == area_PA[ i ].pa_num  ){
				cnt = 0;	
				break;
			}
		}
	}
}

static void PA_management( struct super_block *sb )
{
	struct msdos_sb_info *sbi = MSDOS_SB( sb );

//	struct dentry *root_de = sb->s_root;
//	struct inode *root = root_de->d_inode;
//	struct fat_slot_info sinfo;
	struct msdos_inode_info *ms_i;
	struct hlist_head *head = sbi->inode_hashtable;
	
	printk("[cheon] PA_management Test \n");

	int i;
	
	for( i = 1 ; i < (TOTAL_AREA_CNT-1) ; i++ ) // 현재 4개만 normal, event, parking. manual
	{
		area_PA[ i ].pa_num = ( ( sbi->bx_end_cluster[ i ] - sbi->bx_start_cluster[ i ] + 1 ) ) /  ((sbi->bx_pre_size[ i ] * 1024) / (sbi->cluster_size / 1024)); //각 영역의 PA개수
		area_PA[ i ].pa_num -= 1; //1개 뺴줌
		area_PA[ i ].pa_cluster_num = ( sbi->bx_pre_size[ i ] * 1024 ) / ( sbi->cluster_size / 1024 );
		area_PA[ i ].cur_pa_cnt = 0; 
		area_PA[ i ].active_pa_cnt = 0;

		printk("[cheon] area[%d].pa_num : %d \n", i, area_PA[ i ].pa_num );

		area_PA[ i ].pa_unit = ( struct PA_unit_t  *)kmalloc( sizeof( struct PA_unit_t ) * area_PA[ i ].pa_num, GFP_KERNEL ); 
		memset( ( void *)area_PA[ i ].pa_unit, 0x0, sizeof( struct PA_unit_t ) * area_PA[ i ].pa_num );

		sbi->parea_PA[i]  = &area_PA[i];
	}

	setting_start_end_in_memory( sb );

	for( i = 1 ; i < (TOTAL_AREA_CNT -1) ; i++ )
		decide_each_pa_status( sb, area_PA[ i ].pa_unit, i );


	sbi->bx_free_clusters[ BB_ETC ]			 = 0;
	sbi->bx_free_clusters[ BB_NORMAL ]		 = 0;
	sbi->bx_free_clusters[ BB_NORMAL_EVENT ] = 0;
	sbi->bx_free_clusters[ BB_PARKING ]		 = 0;
	sbi->bx_free_clusters[ BB_MANUAL ]		 = 0;
	sbi->bx_free_clusters[ BB_IMAGE ] 		 = 0;

	sbi->bx_free_valid = -1;
	fat_count_free_clusters_for_area( sb ); //free_cluster 다시 계산
	
	for( i = 1 ; i < (TOTAL_AREA_CNT -1) ; i++ )
		show_the_status_unit_flag( sb, i );





}

static int vfat_fill_super(struct super_block *sb, void *data, int silent)
{
	int res;	
	int i;
	
	printk( "[cheon] vfat_fill_super  S_ID : %s\n", sb->s_id  );
	res = fat_fill_super(sb, data, silent, 1, setup);
	struct msdos_sb_info *sbi = MSDOS_SB(sb); //여기에 있어야 함
	
	if( !strcmp( sb->s_id, SD1_S_ID ) || !strcmp( sb->s_id, SD2_S_ID ) ) //if this device is SD card 
	{
		printk("SD_card\n");

		fat_config_init( sb );
		fat_update_super( sb );

		g_sb_s1 = NULL;
		g_sb_s2 = NULL;

		if( sbi->fat_original_flag == ON ) //ORIGINAL FAT //특정 영역 공간 부족
		{
			if( !strcmp( sb->s_id, SD1_S_ID ) )
				strcpy( manager_control_sd1, "START_ORIGINAL" );
			else
				strcpy( manager_control_sd2, "START_ORIGINAL" );
		}
		else  //OPEL
		{

			if( !strcmp( sb->s_id, SD1_S_ID ) )
			{
				strcpy( manager_control_sd1, "START_OPEL" );
				
				PA_management( sb );
			}
			else
			{
				strcpy( manager_control_sd2, "START_OPEL" );
				
				PA_management( sb );
			}
		}

		if( !strcmp( sb->s_id, SD1_S_ID ) )
		{
			g_sb_s1 = sb;
			if( g_sb_s2 == NULL )
				strcpy( manager_control_sd2, "Not mounted" );


			for(i=0;i<TOTAL_AREA_CNT;i++)	
				g_total_cluster_s1[i] = sbi->bx_end_cluster[i] - sbi->bx_start_cluster[i] + 1;

			//			for(i=0;i<TOTAL_AREA_CNT;i++)	
			//				printk("[cheon] SD1 total free clusters : %u \n", g_total_cluster_s1[i] );
		}
		else
		{
			g_sb_s2 = sb;
			if( g_sb_s1 == NULL )
				strcpy( manager_control_sd1, "Not mounted" );


			for(i=0;i<TOTAL_AREA_CNT;i++)	
				g_total_cluster_s2[i] = sbi->bx_end_cluster[i] - sbi->bx_start_cluster[i] + 1;

			//			for(i=0;i<TOTAL_AREA_CNT;i++)	
			//				printk("[cheon] SD2 total free clusters : %u \n", g_total_cluster_s2[i] );

		}

		printk( KERN_ALERT "manager_control_sd1 : %s \n", manager_control_sd1 );
		printk( KERN_ALERT "manager_control_sd2 : %s \n", manager_control_sd2 );
	}
	else
	{
		printk("For Android\n");

		fat_just_init_super( sb ); //단지 그냥 초기화
		sbi->fat_original_flag = ON;
	}

	return res;
}

static struct dentry *vfat_mount(struct file_system_type *fs_type,
		       int flags, const char *dev_name,
		       void *data)
{
	printk( KERN_ALERT "[cheon] 0906 1627\n");
	printk( KERN_ALERT "[cheon] vfat_mount !! \n");
	return mount_bdev(fs_type, flags, dev_name, data, vfat_fill_super);
}
////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////
#if 1
static struct kobject *opel_fat_kobj;

static ssize_t control_show_s1( struct kobject *kobj, struct kobj_attribute *attr, char *buff )
{
	if( g_sb_s1 == NULL ){
		printk("[cheon] sd1 is not mounted \n");		
		return 0;
	}
	
	printk( KERN_ALERT "[cheon] sysfs control_show_s1() : %s\n", manager_control_sd1 );
	
	return snprintf( buff, PAGE_SIZE, "%s \n", manager_control_sd1 );
}

static ssize_t control_show_s2( struct kobject *kobj, struct kobj_attribute *attr, char *buff )
{
	if( g_sb_s2 == NULL ){
		printk("[cheon] sd2 is not mounted \n");		
		return 0;
	}
	
	printk( KERN_ALERT "[cheon] sysfs control_show_s2() : %s\n", manager_control_sd2 );
	
	return snprintf( buff, PAGE_SIZE, "%s \n", manager_control_sd2 );
}

static ssize_t size_show_s1( struct kobject *kobj, struct kobj_attribute *attr, char *buff )
{
	int i = 0;
	int cnt = 0;
	unsigned long long used_size[10] = {0,};
	unsigned long long free_size[10] = {0,};
	unsigned long long  total_size[10] = {0,};

	//if( strcmp( g_sb_s1->s_id, SD1_S_ID ) )
	if( g_sb_s1 == NULL )
	{
		printk("[cheon] sd1 is not mounted \n");		
		return 0;
	}
	
	struct msdos_sb_info *sbi = MSDOS_SB( g_sb_s1 );
//	printk( KERN_ALERT "[cheon] sys/fs/, size_monitoring() _sd1\n");

	for(i=0;i<TOTAL_AREA_CNT;i++)
	{
		used_size[i] = (g_total_cluster_s1[i] - sbi->bx_free_clusters[i] ) * (sbi->cluster_size/1024);
		free_size[i] = sbi->bx_free_clusters[i] * (sbi->cluster_size/1024);
		total_size[i] = g_total_cluster_s1[i] * (sbi->cluster_size/1024);
	}

#if 0
	cnt = snprintf( buff, PAGE_SIZE, "%u\t%u\t%u\t%u\t%u\t%u \n%u\t%u\t%u\t%u\t%u\t%u \n%u\t%u\t%u\t%u\t%u\t%u \n", total_size[0], total_size[1],total_size[2], total_size[3], total_size[4], total_size[5], \
																								  used_size[0], used_size[1], used_size[2], used_size[3], used_size[4], used_size[5], \
																								  free_size[0], free_size[1], free_size[2], free_size[3], free_size[4], free_size[5] );
#endif
#if 1
	cnt = snprintf( buff, PAGE_SIZE, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n", total_size[0], total_size[1],total_size[2], total_size[3], total_size[4], total_size[5], \
																								  used_size[0], used_size[1], used_size[2], used_size[3], used_size[4], used_size[5], \
																								  free_size[0], free_size[1], free_size[2], free_size[3], free_size[4], free_size[5] );
#endif

	return cnt;
}

static ssize_t size_show_s2( struct kobject *kobj, struct kobj_attribute *attr, char *buff )
{
	int i = 0;
	int cnt = 0;
	unsigned long used_size[10] = {0,};
	unsigned long free_size[10] = {0,};
	unsigned long total_size[10] = {0,};
	
//	if( strcmp( g_sb_s2->s_id, SD2_S_ID ) )
	if( g_sb_s2 == NULL  )
	{
		printk("[cheon] sd2 is not mounted \n");		
		return 0;
	}

	struct msdos_sb_info *sbi = MSDOS_SB( g_sb_s2 );
//	printk( KERN_ALERT "[cheon] sys/fs/, size_monitoring() _sd2\n");

	for(i=0;i<TOTAL_AREA_CNT;i++)
	{
		used_size[i] = (g_total_cluster_s2[i] -sbi->bx_free_clusters[i] ) * (sbi->cluster_size/1024);
		free_size[i] = sbi->bx_free_clusters[i] * (sbi->cluster_size/1024);
		total_size[i] = g_total_cluster_s2[i] * (sbi->cluster_size/1024);
	}

	cnt = snprintf( buff, PAGE_SIZE, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n%llu\t%llu\t%llu\t%llu\t%llu\t%llu \n", total_size[0], total_size[1],total_size[2], total_size[3], total_size[4], total_size[5], \
																								  used_size[0], used_size[1], used_size[2], used_size[3], used_size[4], used_size[5], \
																								  free_size[0], free_size[1], free_size[2], free_size[3], free_size[4], free_size[5] );

	return cnt;
}
////////////////////////////
static ssize_t control_store( struct kobject *kobj, struct kobj_attribute *attr, const char *buff, size_t count )
{
	int num;

	sscanf( buff, "%d", &num );

	printk( KERN_ALERT "num  : %d \n", num );
	printk( KERN_ALERT "buff : %s \n", buff );

	return count;
}

static struct kobj_attribute control_attr1 = __ATTR( SD1_control, 0644, control_show_s1, control_store );
static struct kobj_attribute control_attr2 = __ATTR( SD2_control, 0644, control_show_s2, control_store );
static struct kobj_attribute control_attr3 = __ATTR( SD1_size_monitoring, 0644, size_show_s1, control_store );
static struct kobj_attribute control_attr4 = __ATTR( SD2_size_monitoring, 0644, size_show_s2, control_store );

//static struct kobj_attribute version_attr = __ATTR_RO( version);
static struct attribute *attributes[ ] = {
	    &control_attr1.attr,
	    &control_attr2.attr,
	    &control_attr3.attr,
	    &control_attr4.attr,
		NULL,
};

static struct attribute_group attr_group = {
	.attrs = attributes,
};

static int do_fs_sysfs_registration( void )
{
	int rc; 
	
	opel_fat_kobj = kobject_create_and_add( "OPEL_FAT", fs_kobj ); 

	if( !opel_fat_kobj ){
		printk( KERN_ERR "Unable to create opel fat attributes \n" );
		goto out;
	}

	rc = sysfs_create_group( opel_fat_kobj, &attr_group );

	if( rc )
	{
		printk( KERN_ERR "Unable to create opel fat attributes \n");
		kobject_put( opel_fat_kobj );
	}

out:
	return rc;
}

static void do_fs_sysfs_unregistration( void )
{
	sysfs_remove_group( opel_fat_kobj, &attr_group );
	
	kobject_put( opel_fat_kobj );
	kobject_del( opel_fat_kobj );
}
#endif

static void opel_kill_block_super( struct super_block *sb )
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int i = 0;
	
	for( i = 0 ; i < ( TOTAL_AREA_CNT - 1 ) ; i++ )
		kfree( area_PA[ i ].pa_unit );


	if( sbi->fat_original_flag != ON )
	{
		if( !strcmp( sb->s_id, SD1_S_ID ) )
		{
			g_sb_s1 = NULL;
			strcpy( manager_control_sd1, "Not mounted" );
		}
		else
		{
			g_sb_s2 = NULL;
			strcpy( manager_control_sd2, "Not mounted" );
		}
	}
	
	kill_block_super( sb );
}

/////////////////////
static struct file_system_type vfat_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "vfat",
	.mount		= vfat_mount,
	//.kill_sb	= kill_block_super,
	.kill_sb	= opel_kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("vfat");

static int __init init_vfat_fs(void)
{
#if 1
	int rc = 0;
	printk( KERN_ALERT "[cheon] FAT sysfs Test!! \n");
  
	rc = do_fs_sysfs_registration();

	if( rc ){
		printk( KERN_ALERT "[cheon] sysfs registration faild \b");
	}
#endif

	return register_filesystem(&vfat_fs_type);
}

static void __exit exit_vfat_fs(void)
{
//	printk("[cheon] bye\n");
	do_fs_sysfs_unregistration();
	unregister_filesystem(&vfat_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VFAT filesystem support");
MODULE_AUTHOR("Gordon Chaffee");

module_init(init_vfat_fs)
module_exit(exit_vfat_fs)
