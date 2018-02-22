#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>

#include "bootimg.h"

off_t file_size(char *filename) {
	struct stat st;
	if(stat(filename, &st))
		exit(1);
	return st.st_size;
}

int append_file(int ofd, char *filename, off_t pos) {
	lseek(ofd, pos, SEEK_SET);
	int fd = open(filename, O_RDONLY);
	int size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	sendfile(ofd, fd, NULL, size);
	close(fd);
	return size;
}

int append_ramdisk(int ofd, off_t pos) {
	if(access("ramdisk-mtk", R_OK) == 0) {
		char buf[512];
		off_t size = file_size("ramdisk.gz");
		memcpy(buf, "\x88\x16\x88\x58", 4);
		uint32_t v = size;
		memcpy(buf+4, &v, sizeof(v)); //Should convert to LE

		//TODO: RECOVERY OR ROOTFS?
		char str[32];
		memset(str, 0, sizeof(str));
		if(access("ramdisk-mtk-boot", R_OK)==0) {
			strcpy(str, "ROOTFS");
		} else if(access("ramdisk-mtk-recovery", R_OK)==0) {
			strcpy(str, "RECOVERY");
		} else {
			exit(1);
		}
		memcpy(buf+8, str, sizeof(str));

		memset(buf+8+sizeof(str), 0xff, 512-8-sizeof(str));

		pwrite(ofd, buf, sizeof(buf), pos);

		return append_file(ofd, "ramdisk.gz", pos + 512) + 512;
	} else if(access("ramdisk.gz", R_OK) == 0) {
		return append_file(ofd, "ramdisk.gz", pos);
	} else {
		return append_file(ofd, "ramdisk", pos);
	}
}

void post_process(struct boot_img_hdr *hdr, int ofd, int pos) {
	if(access("rkcrc", R_OK) == 0) {
		fprintf(stderr, "Rockchip CRCs not supported yet\n");
		exit(1);
	}
	//Round up the file size
	ftruncate(ofd, pos);
}

int main(int argc, char **argv) {
	if(argc != 2) {
		fprintf(stderr, "%s <original boot.img> #This program will take local 'kernel' 'second' 'dt' 'ramdisk' files\n", argv[0]);
	}

	//TODO: Merge with extract.c?
	//{
	int ifd = open(argv[1], O_RDONLY);
	off_t isize = lseek(ifd, 0, SEEK_END);
	lseek(ifd, 0, SEEK_SET);
	uint8_t *iorig = mmap(NULL, isize, PROT_READ, MAP_SHARED, ifd, 0);
	uint8_t *ibase = iorig;
	assert(ibase);

	int phsize = 0;

	// Support for Nook Tablet, HD, and HD+
	if (memcmp(ibase + 48, "BauwksBoot", 10) == 0) {
		phsize = 262144;
	} else if (memcmp(ibase + 64, "Green Loader", 12) == 0 ||
			memcmp(ibase + 64, "eMMC boot.img+secondloader", 26) == 0) {
		phsize = 1048576;
	}
	ibase += phsize;

	while(ibase<(iorig+isize)) {
		if(memcmp(ibase, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0)
			break;
		ibase += 256;
	}
	assert(ibase < (iorig+isize));
	//}
	//
	struct boot_img_hdr *ihdr = (struct boot_img_hdr*) ibase;
	assert(
			ihdr->page_size == 2048 ||
			ihdr->page_size == 4096 ||
			ihdr->page_size == 16384
			);

	unlink("new-boot.img");
	int ofd = open("new-boot.img", O_RDWR|O_CREAT, 0644);
	if (phsize > 0) {
		//Write back original preheader
		write(ofd, iorig, phsize);
	}
	ftruncate(ofd, ihdr->page_size + phsize);
	//Write back original header, we'll change it later
	write(ofd, ihdr, sizeof(*ihdr));

	struct boot_img_hdr *hdr = mmap(NULL, sizeof(*ihdr), PROT_READ|PROT_WRITE, MAP_SHARED, ofd, phsize);
	//First set everything to zero, so we know where we are at.
	hdr->kernel_size = 0;
	hdr->ramdisk_size = 0;
	hdr->second_size = 0;
	hdr->unused[0] = 0;
	memset(hdr->id, 0, sizeof(hdr->id)); //Setting id to 0 might be wrong?

	int pos = hdr->page_size + phsize;
	fprintf(stderr, "pos1: [%d]\n", pos);
	int size = 0;

	size = append_file(ofd, "kernel", pos);
	pos += size + hdr->page_size - 1;
	fprintf(stderr, "posa: [%d]\n", pos);
	pos &= ~(hdr->page_size-1);
	fprintf(stderr, "pos2: [%d]\n", pos);
	hdr->kernel_size = size;

	size = append_ramdisk(ofd, pos);
	pos += size + hdr->page_size - 1;
	fprintf(stderr, "posb: [%d]\n", pos);
	pos &= ~(hdr->page_size-1);
	fprintf(stderr, "pos3: [%d]\n", pos);
	hdr->ramdisk_size = size;

	if(access("second", R_OK) == 0) {
		size = append_file(ofd, "second", pos);
		pos += size + hdr->page_size - 1;
		pos &= ~(hdr->page_size-1);
		hdr->second_size = size;
	}

	if(access("dt", R_OK) == 0) {
		size = append_file(ofd, "dt", pos);
		pos += size + hdr->page_size - 1;
		pos &= ~(hdr->page_size-1);
		hdr->unused[0] = size;
	}

	fprintf(stderr, "posf: [%d]\n", pos);
	post_process(hdr, ofd, pos);
	munmap(hdr, sizeof(*ihdr));
	close(ofd);

	return 0;
}
