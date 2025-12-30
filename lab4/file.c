#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include "osfs.h"

static struct osfs_extent_header *
osfs_get_extent_header(struct osfs_sb_info *sb_info,
		       struct osfs_inode *osfs_inode)
{
	if (!sb_info || !osfs_inode)
		return NULL;

	return (struct osfs_extent_header *)(sb_info->data_blocks +
					     osfs_inode->i_block * BLOCK_SIZE);
}

static struct osfs_extent *osfs_get_extent_array(struct osfs_extent_header *hdr)
{
	return (struct osfs_extent *)(hdr + 1);
}

static void osfs_extent_reset_header(struct osfs_extent_header *hdr)
{
	if (!hdr)
		return;
	memset(hdr, 0, BLOCK_SIZE);
	hdr->magic = OSFS_EXTENT_MAGIC;
	hdr->count = 0;
}

static uint32_t osfs_extent_total_blocks(struct osfs_extent_header *hdr)
{
	struct osfs_extent *ext;
	uint32_t i, total = 0;

	if (!hdr || hdr->magic != OSFS_EXTENT_MAGIC)
		return 0;

	if (hdr->count > OSFS_MAX_EXTENTS)
		return 0;

	ext = osfs_get_extent_array(hdr);
	for (i = 0; i < hdr->count; i++)
		total += ext[i].len;

	return total;
}

static int osfs_extent_lookup_block(struct osfs_extent_header *hdr,
				    uint32_t logical_idx,
				    struct osfs_extent **found,
				    uint32_t *phys_block,
				    uint32_t *offset_in_ext)
{
	uint32_t idx = logical_idx;
	struct osfs_extent *ext;
	uint32_t i;

	if (!hdr || hdr->magic != OSFS_EXTENT_MAGIC)
		return -ENOENT;

	if (hdr->count > OSFS_MAX_EXTENTS)
		return -EIO;

	ext = osfs_get_extent_array(hdr);

	for (i = 0; i < hdr->count; i++) {
		if (idx < ext[i].len) {
			if (phys_block)
				*phys_block = ext[i].start + idx;
			if (offset_in_ext)
				*offset_in_ext = idx;
			if (found)
				*found = &ext[i];
			return 0;
		}
		idx -= ext[i].len;
	}

	return -ENOENT;
}

static int osfs_extent_append_block(struct osfs_sb_info *sb_info,
				    struct osfs_inode *osfs_inode,
				    struct inode *inode,
				    struct osfs_extent_header *hdr,
				    uint32_t *new_block)
{
	struct osfs_extent *ext;
	uint32_t block;
	int ret;

	if (!sb_info || !osfs_inode || !hdr)
		return -EIO;

	if (hdr->count > OSFS_MAX_EXTENTS)
		return -ENOSPC;

	ext = osfs_get_extent_array(hdr);

	ret = osfs_alloc_data_block(sb_info, &block);
	if (ret)
		return ret;

	if (hdr->count > 0 &&
	    ext[hdr->count - 1].start + ext[hdr->count - 1].len == block) {
		ext[hdr->count - 1].len++;
	} else {
		if (hdr->count >= OSFS_MAX_EXTENTS) {
			osfs_free_data_block(sb_info, block);
			return -ENOSPC;
		}
		ext[hdr->count].start = block;
		ext[hdr->count].len = 1;
		hdr->count++;
	}

	osfs_inode->i_blocks++;
	inode->i_blocks = osfs_inode->i_blocks;
	if (new_block)
		*new_block = block;
	return 0;
}

static int osfs_extent_ensure_table(struct osfs_sb_info *sb_info,
				    struct osfs_inode *osfs_inode,
				    struct inode *inode,
				    struct osfs_extent_header **hdr_out)
{
	int ret;

	if (!sb_info || !osfs_inode || !inode || !hdr_out)
		return -EIO;

	if (osfs_inode->i_blocks == 0 || osfs_inode->i_block == 0) {
		ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block);
		if (ret)
			return ret;
		osfs_inode->i_blocks = 1;
		inode->i_blocks = osfs_inode->i_blocks;
	}

	*hdr_out = osfs_get_extent_header(sb_info, osfs_inode);
	if (!*hdr_out)
		return -EIO;

	if ((*hdr_out)->magic != OSFS_EXTENT_MAGIC ||
	    (*hdr_out)->count > OSFS_MAX_EXTENTS)
		osfs_extent_reset_header(*hdr_out);

	return 0;
}

static ssize_t osfs_extent_do_read(struct osfs_sb_info *sb_info,
				   struct osfs_inode *osfs_inode,
				   struct inode *inode, char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct osfs_extent_header *hdr;
	size_t remaining = len;
	loff_t pos = *ppos;
	ssize_t done = 0;
	uint32_t logical_block;
	uint32_t block_offset;
	uint32_t phys_block;
	void *block_addr;
	int ret;

	if (!osfs_inode || !sb_info)
		return -EIO;

	if (osfs_inode->i_blocks == 0 || osfs_inode->i_block == 0)
		return 0;

	hdr = osfs_get_extent_header(sb_info, osfs_inode);
	if (!hdr)
		return -EIO;

	if (hdr->magic != OSFS_EXTENT_MAGIC) {
		if (osfs_inode->i_size == 0) {
			osfs_extent_reset_header(hdr);
			return 0;
		}
		return -EOPNOTSUPP;
	}

	if (pos >= osfs_inode->i_size)
		return 0;

	if (pos + remaining > osfs_inode->i_size)
		remaining = osfs_inode->i_size - pos;

	while (remaining > 0) {
		logical_block = pos / BLOCK_SIZE;
		block_offset = pos % BLOCK_SIZE;

		ret = osfs_extent_lookup_block(hdr, logical_block, NULL,
					       &phys_block, NULL);
		if (ret)
			return -EIO;

		block_addr = sb_info->data_blocks +
			     phys_block * BLOCK_SIZE + block_offset;

		{
			size_t chunk = min_t(size_t, remaining,
					     BLOCK_SIZE - block_offset);
			ret = copy_to_user(buf + done, block_addr, chunk);
			if (ret)
				return -EFAULT;
			done += chunk;
			pos += chunk;
			remaining -= chunk;
		}
	}

	*ppos = pos;
	return done;
}

static ssize_t osfs_extent_do_write(struct osfs_sb_info *sb_info,
				    struct osfs_inode *osfs_inode,
				    struct inode *inode,
				    const char __user *buf, size_t len,
				    loff_t *ppos)
{
	struct osfs_extent_header *hdr;
	size_t remaining = len;
	loff_t pos = *ppos;
	ssize_t done = 0;
	uint32_t logical_block;
	uint32_t block_offset;
	uint32_t phys_block;
	uint32_t total_blocks;
	int ret;

	ret = osfs_extent_ensure_table(sb_info, osfs_inode, inode, &hdr);
	if (ret)
		return ret;

	total_blocks = osfs_extent_total_blocks(hdr);

	while (remaining > 0) {
		logical_block = pos / BLOCK_SIZE;
		block_offset = pos % BLOCK_SIZE;

		ret = osfs_extent_lookup_block(hdr, logical_block, NULL,
					       &phys_block, NULL);
		if (ret) {
			if (logical_block > total_blocks)
				return -EINVAL;

			ret = osfs_extent_append_block(sb_info, osfs_inode,
						       inode, hdr,
						       &phys_block);
			if (ret)
				return ret;
			total_blocks++;

			ret = osfs_extent_lookup_block(hdr, logical_block, NULL,
						       &phys_block, NULL);
			if (ret)
				return ret;
		}

		{
			size_t chunk = min_t(size_t, remaining,
					     BLOCK_SIZE - block_offset);
			void *block_addr = sb_info->data_blocks +
					   phys_block * BLOCK_SIZE +
					   block_offset;
			ret = copy_from_user(block_addr, buf + done, chunk);
			if (ret)
				return -EFAULT;
			done += chunk;
			pos += chunk;
			remaining -= chunk;
		}
	}

	if (pos > osfs_inode->i_size) {
		osfs_inode->i_size = pos;
		inode->i_size = osfs_inode->i_size;
	}

	osfs_inode->__i_mtime = osfs_inode->__i_ctime = current_time(inode);
	inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
	inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);
	mark_inode_dirty(inode);

	*ppos = pos;
	return done;
}

/**
 * Function: osfs_read
 * Description: Reads data from a file.
 * Inputs:
 *   - filp: The file pointer representing the file to read from.
 *   - buf: The user-space buffer to copy the data into.
 *   - len: The number of bytes to read.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes read on success.
 *   - 0 if the end of the file is reached.
 *   - -EFAULT if copying data to user space fails.
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read;

    if (S_ISREG(inode->i_mode)) {
        ssize_t extent_ret =
            osfs_extent_do_read(sb_info, osfs_inode, inode, buf, len, ppos);
        if (extent_ret != -EOPNOTSUPP)
            return extent_ret;
    }

    // If the file has not been allocated a data block, it indicates the file is empty
    if (osfs_inode->i_blocks == 0)
        return 0;

    if (*ppos >= osfs_inode->i_size)
        return 0;

    if (*ppos + len > osfs_inode->i_size)
        len = osfs_inode->i_size - *ppos;

    data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE + *ppos;
    if (copy_to_user(buf, data_block, len))
        return -EFAULT;

    *ppos += len;
    bytes_read = len;

    return bytes_read;
}


/**
 * Function: osfs_write
 * Description: Writes data to a file.
 * Inputs:
 *   - filp: The file pointer representing the file to write to.
 *   - buf: The user-space buffer containing the data to write.
 *   - len: The number of bytes to write.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes written on success.
 *   - -EFAULT if copying data from user space fails.
 *   - Adjusted length if the write exceeds the block size.
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{   
    //Step1: Retrieve the inode and filesystem information
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written;
    int ret;

    if (S_ISREG(inode->i_mode)) {
        ssize_t extent_ret =
            osfs_extent_do_write(sb_info, osfs_inode, inode, buf, len, ppos);
        if (extent_ret != -EOPNOTSUPP)
            return extent_ret;
    }

    // Step2: Check if a data block has been allocated; if not, allocate one
    if (!osfs_inode || !sb_info)
        return -EIO;

    if (osfs_inode->i_blocks == 0) {
        ret = osfs_alloc_data_block(sb_info, &osfs_inode->i_block);
        if (ret)
            return ret;
        osfs_inode->i_blocks = 1;
        inode->i_blocks = osfs_inode->i_blocks;
    }


    // Step3: Limit the write length to fit within one data block
    if (*ppos >= BLOCK_SIZE)
        return -ENOSPC;

    if (*ppos + len > BLOCK_SIZE)
        len = BLOCK_SIZE - *ppos;


    // Step4: Write data from user space to the data block
    data_block = sb_info->data_blocks + osfs_inode->i_block * BLOCK_SIZE + *ppos;

    ret = copy_from_user(data_block, buf, len);
    if (ret)
        return -EFAULT;


    // Step5: Update inode & osfs_inode attribute
    *ppos += len;
    if (*ppos > osfs_inode->i_size) {
        osfs_inode->i_size = *ppos;
        inode->i_size = osfs_inode->i_size;
    }
    osfs_inode->__i_mtime = osfs_inode->__i_ctime = current_time(inode);
    inode_set_mtime_to_ts(inode, osfs_inode->__i_mtime);
    inode_set_ctime_to_ts(inode, osfs_inode->__i_ctime);
    mark_inode_dirty(inode);


    // Step6: Return the number of bytes written
    bytes_written = len;

    
    return bytes_written;
}

/**
 * Struct: osfs_file_operations
 * Description: Defines the file operations for regular files in osfs.
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open, // Use generic open or implement osfs_open if needed
    .read = osfs_read,
    .write = osfs_write,
    .llseek = default_llseek,
    // Add other operations as needed
};

/**
 * Struct: osfs_file_inode_operations
 * Description: Defines the inode operations for regular files in osfs.
 * Note: Add additional operations such as getattr as needed.
 */
const struct inode_operations osfs_file_inode_operations = {
    // Add inode operations here, e.g., .getattr = osfs_getattr,
};
