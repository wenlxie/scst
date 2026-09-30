/* Issue one 4 KiB SCSI READ(10) through a scsi_generic device. */
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned char cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 8, 0 };
	unsigned char sense[64] = { 0 };
	void *buf;
	sg_io_hdr_t io = { 0 };
	int fd;
	unsigned long lba;
	int attempt;

	if (argc != 3) {
		fprintf(stderr, "usage: %s /dev/sgN LBA\n", argv[0]);
		return 2;
	}
	lba = strtoul(argv[2], NULL, 0);
	if (lba > 4096 - 8)
		return 2;
	cdb[2] = lba >> 24;
	cdb[3] = lba >> 16;
	cdb[4] = lba >> 8;
	cdb[5] = lba;
	if (posix_memalign(&buf, 4096, 4096) != 0)
		return 2;
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 2;
	}
	io.interface_id = 'S';
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.cmd_len = sizeof(cdb);
	io.mx_sb_len = sizeof(sense);
	io.dxfer_len = 4096;
	io.dxferp = buf;
	io.cmdp = cdb;
	io.sbp = sense;
	io.timeout = 10000;
	for (attempt = 0; attempt < 5; attempt++) {
		memset(sense, 0, sizeof(sense));
		if (ioctl(fd, SG_IO, &io) < 0) {
			perror("SG_IO");
			return 1;
		}
		if (!io.status && !io.host_status && !io.driver_status)
			break;
		fprintf(stderr, "%s: attempt %d status=0x%x sense_key=0x%x asc=0x%x ascq=0x%x\n",
			argv[1], attempt + 1, io.status, sense[2] & 0xf,
			sense[12], sense[13]);
		usleep(100000);
	}
	printf("%s: status=0x%x host_status=0x%x driver_status=0x%x\n",
		argv[1], io.status, io.host_status, io.driver_status);
	close(fd);
	free(buf);
	return io.status || io.host_status || io.driver_status;
}
