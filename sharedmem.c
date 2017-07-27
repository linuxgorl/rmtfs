#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rmtfs.h"

static int rmtfs_mem_enumerate(struct rmtfs_mem *rmem);

struct rmtfs_mem {
	uint64_t address;
	uint64_t size;
	void *base;
	int fd;
};

struct rmtfs_mem *rmtfs_mem_open(void)
{
	struct rmtfs_mem *rmem;
	void *base;
	int ret;
	int fd;

	rmem = malloc(sizeof(*rmem));
	if (!rmem)
		return NULL;

	ret = rmtfs_mem_enumerate(rmem);
	if (ret < 0)
		goto err;

	fd = open("/dev/mem", O_RDWR|O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "failed to open /dev/mem\n");
		goto err;
	}

	base = mmap(0, rmem->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, rmem->address);
	if (base == MAP_FAILED) {
		fprintf(stderr, "failed to mmap: %s\n", strerror(errno));
		goto err_close_fd;
	}

	rmem->base = base;
	rmem->fd = fd;

	return rmem;

err_close_fd:
	close(fd);
err:
	free(rmem);
	return NULL;
}

int64_t rmtfs_mem_alloc(struct rmtfs_mem *rmem, size_t alloc_size)
{
	if (alloc_size > rmem->size) {
		fprintf(stderr,
			"[RMTFS] rmtfs shared memory not large enough for allocation request 0x%zx vs 0x%lx\n",
			alloc_size, rmem->size);
		return -EINVAL;
	}

	return rmem->address;
}

void rmtfs_mem_free(struct rmtfs_mem *rmem)
{
}

void *rmtfs_mem_ptr(struct rmtfs_mem *rmem, unsigned phys_address, size_t len)
{
	uint64_t start;
	uint64_t end;

	start = phys_address;
	end = start + len;

	if (start < rmem->address || end > rmem->address + rmem->size)
		return NULL;

	return rmem->base + phys_address - rmem->address;
}

void rmtfs_mem_close(struct rmtfs_mem *rmem)
{
	munmap(rmem->base, rmem->size);
	close(rmem->fd);

	rmem->fd = -1;
	rmem->base = MAP_FAILED;
}

static int rmtfs_mem_enumerate(struct rmtfs_mem *rmem)
{
	union {
		uint32_t dw[2];
		uint64_t qw[2];
	} reg;
	struct dirent *de;
	int basefd;
	int dirfd;
	int regfd;
	DIR *dir;
	int ret = 0;
	int n;

	basefd = open("/proc/device-tree/reserved-memory/", O_DIRECTORY);
	dir = fdopendir(basefd);
	if (!dir) {
		fprintf(stderr,
			"Unable to open reserved-memory device tree node: %s\n",
			strerror(-errno));
		close(basefd);
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "rmtfs", 5) != 0)
			continue;

		dirfd = openat(basefd, de->d_name, O_DIRECTORY);
		if (dirfd < 0) {
			fprintf(stderr, "failed to open %s: %s\n",
				de->d_name, strerror(-errno));
			ret = -1;
			goto out;
		}

		regfd = openat(dirfd, "reg", O_RDONLY);
		if (regfd < 0) {
			fprintf(stderr, "failed to open reg of %s: %s\n",
				de->d_name, strerror(-errno));
			ret = -1;
			goto out;
		}

		n = read(regfd, &reg, sizeof(reg));
		if (n == 2 * sizeof(uint32_t)) {
			rmem->address = be32toh(reg.dw[0]);
			rmem->size = be32toh(reg.dw[1]);
		} else if (n == 2 * sizeof(uint64_t)) {
			rmem->address = be64toh(reg.qw[0]);
			rmem->size = be64toh(reg.qw[1]);
		} else {
			fprintf(stderr, "failed to read reg of %s: %s\n",
				de->d_name, strerror(-errno));
			ret = -1;
		}

		close(regfd);
		close(dirfd);
		break;
	}

out:
	closedir(dir);
	close(basefd);
	return ret;
}
