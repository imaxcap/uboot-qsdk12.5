/*
 * Copyright (c) 2015-2017, 2020 The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <command.h>
#include <bootm.h>
#include <image.h>
#include <nand.h>
#include <errno.h>
#include <asm/arch-qca-common/scm.h>
#include <part.h>
#include <ubi_uboot.h>
#include <asm/arch-qca-common/smem.h>
#include <mmc.h>
#include <part_efi.h>
#include <fdtdec.h>
#include <libfdt.h>
#include "fdt_info.h"
#include <asm/errno.h>
#include <asm/arch-qca-common/qca_common.h>
#include <usb.h>
#include <elf.h>
#include <cli.h>
#include <ipq_api.h>

#define SEC_AUTH_SW_ID 		0x17
#define ROOTFS_IMAGE_TYPE       0x13
#define NO_OF_PROGRAM_HDRS	3
#define ELF_HDR_PLUS_PHDR_SIZE	sizeof(Elf32_Ehdr) + \
		(NO_OF_PROGRAM_HDRS * sizeof(Elf32_Phdr))

#define PRIMARY_PARTITION	1
#define SECONDARY_PARTITION	2

#define FIT_BOOTARGS_PROP	"append-bootargs"

extern int qca_scm_part_info(void *cmd_buf, size_t cmd_len);

unsigned long __stack_chk_guard = 0x000a0dff;
static int debug = 0;
static char mtdids[256];
DECLARE_GLOBAL_DATA_PTR;
static qca_smem_flash_info_t *sfi = &qca_smem_flash_info;
int ipq_fs_on_nand ;
extern int nand_env_device;
extern qca_mmc mmc_host;
extern void set_minidump_bootargs(void);

#ifdef CONFIG_QCA_MMC
static qca_mmc *host = &mmc_host;
#endif

typedef struct {
	unsigned int image_type;
	unsigned int header_vsn_num;
	unsigned int image_src;
	unsigned char *image_dest_ptr;
	unsigned int image_size;
	unsigned int code_size;
	unsigned char *signature_ptr;
	unsigned int signature_size;
	unsigned char *cert_chain_ptr;
	unsigned int cert_chain_size;
} mbn_header_t;

typedef struct {
	unsigned int kernel_load_addr;
	unsigned int kernel_load_size;
} kernel_img_info_t;

kernel_img_info_t kernel_img_info;

char dtb_config_name[64];

#ifdef CONFIG_IPQ_ELF_AUTH
typedef struct {
	unsigned int img_offset;
	unsigned int img_load_addr;
	unsigned int img_size;
} image_info;
#endif

extern bootm_headers_t images;		/* pointers to os/initrd/fdt images */
#ifdef CONFIG_LIST_OF_CONFIG_NAMES_SUPPORT
extern struct config_list config_entries;
#endif

static int boot_os(int argc, char *const argv[])
{

	return do_bootm_states(NULL, 0, argc, argv, BOOTM_STATE_START |
		BOOTM_STATE_FINDOS | BOOTM_STATE_FINDOTHER |
		BOOTM_STATE_LOADOS |
#if defined(CONFIG_PPC) || defined(CONFIG_MIPS)
		BOOTM_STATE_OS_CMDLINE |
#endif
		BOOTM_STATE_OS_PREP | BOOTM_STATE_OS_FAKE_GO |
		BOOTM_STATE_OS_GO, &images, 1);
}

void __stack_chk_fail(void)
{
	printf("stack-protector: U-boot stack is corrupted.\n");
	bad_mode ();
}

static int update_bootargs(void *addr)
{
	char *fit_bootargs, *strings;
	int len, ret = CMD_RET_SUCCESS;
	char * cmd_line = malloc(CONFIG_SYS_CBSIZE);
	if (!cmd_line) {
		printf("bootargs malloc failed \n");
		return CMD_RET_FAILURE;
	}

#ifdef CONFIG_QCA_APPSBL_DLOAD
	if (getenv("dump_to_nvmem"))
		set_minidump_bootargs();
#endif

       	strings = getenv("bootargs");
	memset(cmd_line, 0, CONFIG_SYS_CBSIZE);
	fit_bootargs = (char *)fdt_getprop(addr, 0, FIT_BOOTARGS_PROP, &len);
	if ((fit_bootargs != NULL) && len) {
		if (strings && ((strlen(strings) + len) > CONFIG_SYS_CBSIZE)) {
			ret = CMD_RET_FAILURE;
		} else {
			if(strings)
				memcpy(cmd_line, strings, strlen(strings));

			snprintf(cmd_line + (strings? strlen(strings) : 0),
					CONFIG_SYS_CBSIZE,
					" %s rootwait", fit_bootargs);
		}
	} else {
		if(strings)
			memcpy(cmd_line, strings, strlen(strings));

		len = snprintf(cmd_line + (strings? strlen(strings) : 0),
				CONFIG_SYS_CBSIZE,
				" %s rootwait", getenv("fsbootargs"));
		if (len >= CONFIG_SYS_CBSIZE)
			ret = CMD_RET_FAILURE;
	}

	if (ret == CMD_RET_FAILURE) {
		printf("Env size exceed ...\n");
	} else {
		setenv("bootargs", NULL);
		setenv("bootargs", cmd_line);
	}

	free(cmd_line);
	return ret;
}
/*
 * Set the root device and bootargs for mounting root filesystem.
 */
static int set_fs_bootargs(void)
{
	char *bootargs;
	unsigned int active_part = 0;
	int ret = 0;
	char boot_args[MAX_BOOT_ARGS_SIZE] = {'\0'};



#define nand_rootfs "ubi.mtd=" QCA_ROOT_FS_PART_NAME " root=mtd:ubi_rootfs rootfstype=squashfs"

	if (sfi->flash_type == SMEM_BOOT_SPI_FLASH) {
		if (get_which_flash_param("rootfs") ||
		    ((sfi->flash_secondary_type == SMEM_BOOT_NAND_FLASH) ||
			(sfi->flash_secondary_type == SMEM_BOOT_QSPI_NAND_FLASH))) {
			bootargs = nand_rootfs;
			ipq_fs_on_nand = 1;

			if (sfi->rootfs.offset == 0xBAD0FF5E) {
				if(smem_bootconfig_info() == 0)
					active_part = get_rootfs_active_partition();

				sfi->rootfs.offset = active_part * IPQ_NAND_ROOTFS_SIZE;
				sfi->rootfs.size = IPQ_NAND_ROOTFS_SIZE;
			}

			fdt_setprop((void *)gd->fdt_blob, 0, "nor_nand_available",
				    &ipq_fs_on_nand, sizeof(ipq_fs_on_nand));
			snprintf(mtdids, sizeof(mtdids),
				 "nand%d=nand%d,nand%d=" QCA_SPI_NOR_DEVICE,
				 is_spi_nand_available(),
				 is_spi_nand_available(),
				CONFIG_SPI_FLASH_INFO_IDX
				);

			if (getenv("fsbootargs") == NULL)
				setenv("fsbootargs", bootargs);
		} else {
			bootargs = boot_args;
			if (smem_bootconfig_info() == 0) {
				active_part = get_rootfs_active_partition();
				// TODO: AP8220 rootfs1 rootfs2
				if (active_part) {
					strlcpy(bootargs, "rootfsname=rootfs_1", sizeof(boot_args));
					if (sfi->rootfs.offset == 0xBAD0FF5E)
						ret  = set_uuid_bootargs(bootargs, "rootfs_1", sizeof(boot_args), false);
				} else {
					strlcpy(bootargs, "rootfsname=rootfs", sizeof(boot_args));
					if (sfi->rootfs.offset == 0xBAD0FF5E)
						ret  = set_uuid_bootargs(bootargs, "rootfs", sizeof(boot_args), false);
				}
			} else {
				strlcpy(bootargs, "rootfsname=rootfs", sizeof(boot_args));
				if (sfi->rootfs.offset == 0xBAD0FF5E)
					ret  = set_uuid_bootargs(bootargs, "rootfs", sizeof(boot_args), false);
			}

			if (ret)
				return ret;

			ipq_fs_on_nand = 0;

			snprintf(mtdids, sizeof(mtdids), "nand%d="
				QCA_SPI_NOR_DEVICE, CONFIG_SPI_FLASH_INFO_IDX);

			if (getenv("fsbootargs") == NULL)
				setenv("fsbootargs", bootargs);
		}
	} else if (((sfi->flash_type == SMEM_BOOT_NAND_FLASH) ||
			(sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH))) {
		bootargs = nand_rootfs;
		if (getenv("fsbootargs") == NULL)
			setenv("fsbootargs", bootargs);
		ipq_fs_on_nand = 1;

		snprintf(mtdids, sizeof(mtdids), "nand0=nand0");

#ifdef CONFIG_QCA_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH) {
		bootargs = boot_args;
		if (smem_bootconfig_info() == 0) {
			active_part = get_rootfs_active_partition();
			if (active_part) {
				strlcpy(bootargs, "rootfsname=rootfs_1 gpt", sizeof(boot_args));
				ret  = set_uuid_bootargs(bootargs, "rootfs_1", sizeof(boot_args), true);
			} else {
				strlcpy(bootargs, "rootfsname=rootfs gpt", sizeof(boot_args));
				bootargs = "rootfsname=rootfs gpt";
				ret  = set_uuid_bootargs(bootargs, "rootfs", sizeof(boot_args), true);
			}
		} else {
			strlcpy(bootargs, "rootfsname=rootfs gpt", sizeof(boot_args));
			ret  = set_uuid_bootargs(bootargs, "rootfs", sizeof(boot_args), true);
		}

		if (ret)
			return ret;

		ipq_fs_on_nand = 0;
		if (getenv("fsbootargs") == NULL)
			setenv("fsbootargs", bootargs);
#endif
	} else {
		printf("bootipq: unsupported boot flash type\n");
		return -EINVAL;
	}
	return 0;
}

static int config_select_internal(unsigned int addr, char *rcmd,
		int rcmd_size, int update_args)
{
	/* Selecting a config name from the list of available
	 * config names by passing them to the fit_conf_get_node()
	 * function which is used to get the node_offset with the
	 * config name passed. Based on the return value index based
	 * or board name based config is used.
	 */

#ifdef CONFIG_ARCH_IPQ806x
	int soc_version = 0;
#endif
	int i, strings_count, ret;
	const char *config = getenv("config_name");

	if (config) {
		printf("Manual device tree config selected!\n");
		strlcpy(dtb_config_name, config, sizeof(dtb_config_name));
		if (fit_conf_get_node((void *)addr, dtb_config_name) >= 0) {
			if (update_args) {
				ret = update_bootargs((void *)addr);
				if (ret)
					goto fail;
			}
			snprintf(rcmd, rcmd_size, "0x%x#%s",
				 addr, dtb_config_name);
			return 0;
		}
	} else {
#ifdef CONFIG_LIST_OF_CONFIG_NAMES_SUPPORT
		strings_count = config_entries.no_of_entries;
#else
		strings_count = fdt_count_strings(gd->fdt_blob, 0,
							"config_name");
#endif

		if (!strings_count) {
			printf("Failed to get config_name\n");
			return -1;
		}

		for (i = 0; i < strings_count; i++) {
#ifdef CONFIG_LIST_OF_CONFIG_NAMES_SUPPORT
			config = config_entries.entry[i];
#else
			fdt_get_string_index(gd->fdt_blob, 0, "config_name",
					   i, &config);
#endif
			snprintf((char *)dtb_config_name,
				 sizeof(dtb_config_name), "%s", config);

#ifdef CONFIG_ARCH_IPQ806x
			ipq_smem_get_socinfo_version((uint32_t *)&soc_version);
			if(SOCINFO_VERSION_MAJOR(soc_version) >= 2) {
				snprintf(dtb_config_name + strlen("config@"),
					 sizeof(dtb_config_name) - strlen("config@"),
					 "v%d.0-%s",
					 SOCINFO_VERSION_MAJOR(soc_version),
					 config + strlen("config@"));
			}
#endif
			if (fit_conf_get_node((void *)addr, dtb_config_name) >= 0) {
				if (update_args) {
					ret = update_bootargs((void *)addr);
					if (ret)
						goto fail;
				}
				snprintf(rcmd, rcmd_size, "0x%x#%s",
					 addr, dtb_config_name);
				return 0;
			}
		}
	}

	printf("Config not available\n");
fail:
	return -1;
}

int config_select(unsigned int addr, char *rcmd, int rcmd_size)
{
	return config_select_internal(addr, rcmd, rcmd_size, 1);
}

__weak int switch_ce_channel_buf(unsigned int channel_id)
{
	return 0;
}

#ifdef CONFIG_IPQ_ELF_AUTH
static int parse_elf_image_phdr_len(image_info *img_info, unsigned int addr,
		size_t size, int verbose)
{
	Elf32_Ehdr *ehdr; /* Elf header structure pointer */
	Elf32_Phdr *phdr; /* Program header structure pointer */
	unsigned int phnum;
	int i;

	if (size < sizeof(*ehdr))
		return -EINVAL;

	ehdr = (Elf32_Ehdr *)addr;

	if (!IS_ELF(*ehdr)) {
		if (verbose)
			printf("It is not a elf image \n");
		return -EINVAL;
	}

	if (ehdr->e_type != ET_EXEC) {
		if (verbose)
			printf("Not a valid elf image\n");
		return -EINVAL;
	}
	phnum = min_t(unsigned int, ehdr->e_phnum, NO_OF_PROGRAM_HDRS);
	if (ehdr->e_phentsize < sizeof(*phdr) ||
	    ehdr->e_phoff > size ||
	    phnum > (size - ehdr->e_phoff) / ehdr->e_phentsize) {
		if (verbose)
			printf("ELF program header table is outside image: "
			       "phoff=0x%x phentsize=0x%x phnum=%u size=0x%zx\n",
			       ehdr->e_phoff, ehdr->e_phentsize,
			       ehdr->e_phnum, size);
		return -EINVAL;
	}

	/* Load each program header */
	for (i = 0; i < phnum; ++i) {
		phdr = (Elf32_Phdr *)(addr + ehdr->e_phoff +
				       (i * ehdr->e_phentsize));
		if (verbose)
			printf("Parsing phdr load addr 0x%x offset 0x%x "
			       "size 0x%x type 0x%x\n", phdr->p_paddr,
			       phdr->p_offset, phdr->p_filesz, phdr->p_type);
		if(phdr->p_type == PT_LOAD) {
			if (phdr->p_offset > (unsigned int)-1 - phdr->p_filesz)
				return -EINVAL;
			img_info->img_offset = phdr->p_offset;
			img_info->img_load_addr = phdr->p_paddr;
			img_info->img_size =  phdr->p_filesz;
			return 0;
		}
	}

	return -EINVAL;
}

static int parse_elf_image_phdr(image_info *img_info, unsigned int addr)
{
	return parse_elf_image_phdr_len(img_info, addr,
					ELF_HDR_PLUS_PHDR_SIZE, 1);
}
#endif

#if (defined(CONFIG_ARCH_IPQ6018) || defined(CONFIG_ARCH_IPQ807x)) && \
	defined(CONFIG_CMD_UBI) && defined(CONFIG_CMD_NAND)

enum ipq_kernel_source_type {
	IPQ_KERNEL_UBI,
	IPQ_KERNEL_RAW_NAND,
};

struct ipq_kernel_source {
	enum ipq_kernel_source_type type;
	const char *partition;
	const char *volume;
};

static const struct ipq_kernel_source ipq_kernel_sources[] = {
	{ IPQ_KERNEL_UBI, "ubi_kernel", "kernel" },
	{ IPQ_KERNEL_UBI, "rootfs", "kernel" },
	{ IPQ_KERNEL_RAW_NAND, "kernel", NULL },
};

#define IPQ_KERNEL_PROBE_SIZE	4096

static const char *ipq_kernel_source_name(const struct ipq_kernel_source *source)
{
	return source->type == IPQ_KERNEL_UBI ? "ubi" : "raw-nand";
}

static int ipq_has_secondary_nand(void)
{
	int i;

	if (sfi->flash_type != SMEM_BOOT_SPI_FLASH)
		return 0;
	if (sfi->flash_secondary_type == SMEM_BOOT_NAND_FLASH ||
	    sfi->flash_secondary_type == SMEM_BOOT_QSPI_NAND_FLASH)
		return 1;

	for (i = 0; i < ARRAY_SIZE(ipq_kernel_sources); i++) {
		if (smem_part_exists(ipq_kernel_sources[i].partition) &&
		    get_which_flash_param((char *)ipq_kernel_sources[i].partition))
			return 1;
	}

	return 0;
}

static int ipq_kernel_memory_check(uintptr_t addr, size_t size)
{
	if (!size || size > (size_t)(~(uintptr_t)0) - addr) {
		printf("bootipq: load region check failed addr=0x%lx size=0x%zx "
		       "check=overflow errno=%d\n", (ulong)addr, size, -EFBIG);
		return -EFBIG;
	}

	if (!is_memory_region_available(addr, size)) {
		printf("bootipq: load region check failed addr=0x%lx size=0x%zx "
		       "check=reserved_or_out_of_ram errno=%d\n",
		       (ulong)addr, size, -EFBIG);
		return -EFBIG;
	}

	return 0;
}

static int ipq_resolve_nand_partition(const char *partition,
		loff_t *offset, size_t *size, int *nand_dev)
{
	uint32_t part_offset = 0;
	uint32_t part_size = 0;
	int table_entry = 1;
	int ret;

	ret = getpart_offset_size((char *)partition, &part_offset, &part_size);
	if (ret) {
		table_entry = 0;
		/* Preserve the legacy synthetic rootfs partition calculation. */
		if (!strcmp(partition, "rootfs") &&
		    sfi->rootfs.offset != 0xBAD0FF5E &&
		    sfi->rootfs.size != 0xBAD0FF5E) {
			if (sfi->rootfs.offset > (uint32_t)-1 ||
			    sfi->rootfs.size > (uint32_t)-1)
				return -ERANGE;
			part_offset = sfi->rootfs.offset;
			part_size = sfi->rootfs.size;
		} else {
			return ret;
		}
	}

	if (!part_size)
		return -EINVAL;

	if (sfi->flash_type == SMEM_BOOT_NAND_FLASH ||
	    sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH) {
		*nand_dev = CONFIG_NAND_FLASH_INFO_IDX;
	} else if (ipq_has_secondary_nand()) {
		if (table_entry && !get_which_flash_param((char *)partition))
			return -ENODEV;
		*nand_dev = is_spi_nand_available();
	} else {
		return -ENODEV;
	}

	if (*nand_dev < 0 || *nand_dev >= CONFIG_SYS_MAX_NAND_DEVICE ||
	    !nand_info[*nand_dev].size)
		return -ENODEV;

	*offset = part_offset;
	*size = part_size;
	if (*offset > nand_info[*nand_dev].size ||
	    *size > nand_info[*nand_dev].size - *offset) {
		printf("bootipq: partition=%s device=nand%d offset=0x%llx "
		       "size=0x%zx device_size=0x%llx check=bounds errno=%d\n",
		       partition, *nand_dev, *offset, *size,
		       nand_info[*nand_dev].size, -EINVAL);
		return -EINVAL;
	}
	printf("bootipq: partition=%s device=nand%d offset=0x%llx "
	       "size=0x%zx\n", partition, *nand_dev, *offset, *size);

	return 0;
}

static int ipq_image_required_at(unsigned int addr, size_t available,
		size_t image_offset, size_t *required)
{
	int format;
	size_t image_size;

	if (image_offset > available ||
	    available - image_offset < sizeof(uint32_t))
		return -EINVAL;

	format = genimg_get_format((void *)(addr + image_offset));
	if (format == IMAGE_FORMAT_FIT) {
		if (available - image_offset < sizeof(struct fdt_header))
			return -EINVAL;
		image_size = fdt_totalsize((void *)(addr + image_offset));
		if (image_size < sizeof(struct fdt_header) ||
		    image_size > (size_t)-1 - image_offset)
			return -EINVAL;
		*required = image_offset + image_size;
		return 0;
	}

	if (format == IMAGE_FORMAT_LEGACY) {
		image_header_t *hdr;

		if (available - image_offset < sizeof(*hdr))
			return -EINVAL;
		hdr = (image_header_t *)(addr + image_offset);
		image_size = image_get_image_size(hdr);
		if (image_size < sizeof(*hdr) ||
		    image_size > (size_t)-1 - image_offset)
			return -EINVAL;
		*required = image_offset + image_size;
		return 0;
	}

	return -ENOEXEC;
}

static int ipq_image_required_size(unsigned int addr, size_t available,
		size_t *required)
{
	int format;

	if (available >= sizeof(uint32_t)) {
		format = genimg_get_format((void *)addr);
		if (format == IMAGE_FORMAT_FIT ||
		    format == IMAGE_FORMAT_LEGACY)
			return ipq_image_required_at(addr, available, 0, required);
	}

	if (available >= sizeof(mbn_header_t) + sizeof(uint32_t)) {
		format = genimg_get_format((void *)(addr + sizeof(mbn_header_t)));
		if (format == IMAGE_FORMAT_FIT ||
		    format == IMAGE_FORMAT_LEGACY)
			return ipq_image_required_at(addr, available,
						     sizeof(mbn_header_t),
						     required);
	}

#ifdef CONFIG_IPQ_ELF_AUTH
	{
		image_info img_info;
		int ret;

		ret = parse_elf_image_phdr_len(&img_info, addr, available, 0);
		if (!ret) {
			if (img_info.img_offset > (size_t)-1 - img_info.img_size)
				return -EINVAL;
			*required = img_info.img_offset + img_info.img_size;
			return 0;
		}
	}
#endif

	return -ENOEXEC;
}

static int ipq_validate_image_at(unsigned int addr, size_t loaded_size,
		size_t image_offset, char *runcmd, int runcmd_size,
		unsigned int *fit_addr)
{
	int format;
	size_t image_size;

	if (image_offset > loaded_size ||
	    loaded_size - image_offset < sizeof(uint32_t))
		return -EINVAL;

	format = genimg_get_format((void *)(addr + image_offset));
	if (format == IMAGE_FORMAT_FIT) {
		int ret;

		if (loaded_size - image_offset < sizeof(struct fdt_header))
			return -EINVAL;
		image_size = fdt_totalsize((void *)(addr + image_offset));
		if (image_size < sizeof(struct fdt_header) ||
		    image_size > loaded_size - image_offset) {
			printf("bootipq: FIT size check failed offset=0x%zx "
			       "fit_size=0x%zx loaded=0x%zx errno=%d\n",
			       image_offset, image_size, loaded_size, -EFBIG);
			return -EFBIG;
		}
		ret = fit_check_format((void *)(addr + image_offset), image_size);
		if (ret)
			return ret;
		ret = config_select_internal(addr + image_offset, runcmd,
					     runcmd_size, 0);
		if (ret)
			return -ENOENT;
		*fit_addr = addr + image_offset;
		return 0;
	}

	if (format == IMAGE_FORMAT_LEGACY) {
		image_header_t *hdr;

		if (loaded_size - image_offset < sizeof(*hdr))
			return -EINVAL;
		hdr = (image_header_t *)(addr + image_offset);
		if (!image_check_magic(hdr) || !image_check_hcrc(hdr))
			return -EBADMSG;
		image_size = image_get_image_size(hdr);
		if (image_size < sizeof(*hdr) ||
		    image_size > loaded_size - image_offset)
			return -EFBIG;
		if (getenv_yesno("verify") != 0 && !image_check_dcrc(hdr))
			return -EBADMSG;
		snprintf(runcmd, runcmd_size, "0x%x", addr + image_offset);
		*fit_addr = 0;
		return 0;
	}

	return -ENOEXEC;
}

static int ipq_prepare_kernel_image(unsigned int addr, size_t loaded_size,
		char *runcmd, int runcmd_size, unsigned int *fit_addr)
{
	int format;

	if (loaded_size >= sizeof(uint32_t)) {
		format = genimg_get_format((void *)addr);
		if (format == IMAGE_FORMAT_FIT ||
		    format == IMAGE_FORMAT_LEGACY)
			return ipq_validate_image_at(addr, loaded_size, 0, runcmd,
						     runcmd_size, fit_addr);
	}

	if (loaded_size >= sizeof(mbn_header_t) + sizeof(uint32_t)) {
		format = genimg_get_format((void *)(addr + sizeof(mbn_header_t)));
		if (format == IMAGE_FORMAT_FIT ||
		    format == IMAGE_FORMAT_LEGACY)
			return ipq_validate_image_at(addr, loaded_size,
						     sizeof(mbn_header_t),
						     runcmd, runcmd_size,
						     fit_addr);
	}

#ifdef CONFIG_IPQ_ELF_AUTH
	{
		image_info img_info;
		int ret;

		ret = parse_elf_image_phdr_len(&img_info, addr, loaded_size, 0);
		if (!ret && img_info.img_offset <= loaded_size &&
		    img_info.img_size <= loaded_size - img_info.img_offset)
			return ipq_validate_image_at(addr, loaded_size,
						     img_info.img_offset,
						     runcmd, runcmd_size,
						     fit_addr);
	}
#endif

	return -ENOEXEC;
}

static int ipq_load_ubi_kernel(const struct ipq_kernel_source *source,
		unsigned int load_addr, size_t *loaded_size,
		const char **failed_stage)
{
	long long volume_size;
	loff_t offset;
	size_t part_size;
	size_t volume_bytes;
	size_t read_size;
	size_t required;
	char mtd_dev[16];
	int nand_dev;
	int ret;

	*failed_stage = "partition";
	ret = ipq_resolve_nand_partition(source->partition, &offset,
					 &part_size, &nand_dev);
	if (ret)
		return ret;

	snprintf(mtd_dev, sizeof(mtd_dev), "nand%d", nand_dev);
	*failed_stage = "attach";
	ret = ubi_part_region(source->partition, mtd_dev, offset, part_size,
			      NULL);
	if (ret)
		goto out;

	*failed_stage = "read";
	volume_size = ubi_get_volume_size((char *)source->volume);
	if (volume_size < 0) {
		ret = (int)volume_size;
		printf("bootipq: UBI volume lookup failed partition=%s "
		       "volume=%s errno=%d\n", source->partition,
		       source->volume, ret);
		goto out;
	}
	if (!volume_size || (unsigned long long)volume_size > (size_t)-1) {
		ret = -EFBIG;
		printf("bootipq: UBI volume size invalid partition=%s "
		       "volume=%s size=%lld errno=%d\n", source->partition,
		       source->volume, volume_size, ret);
		goto out;
	}
	volume_bytes = volume_size;

	read_size = min_t(size_t, volume_bytes, IPQ_KERNEL_PROBE_SIZE);
	ret = ipq_kernel_memory_check(load_addr, read_size);
	if (ret)
		goto out;
	ret = ubi_volume_read((char *)source->volume, (char *)load_addr,
			      read_size);
	if (ret)
		goto out;

	*failed_stage = "image-format";
	ret = ipq_image_required_size(load_addr, read_size, &required);
	if (ret)
		goto out;
	if (required > volume_bytes) {
		ret = -EFBIG;
		printf("bootipq: UBI image size invalid partition=%s volume=%s "
		       "image_size=0x%zx volume_size=0x%llx errno=%d\n",
		       source->partition, source->volume, required,
		       volume_size, ret);
		goto out;
	}
	ret = ipq_kernel_memory_check(load_addr, required);
	if (ret)
		goto out;

	if (required > read_size) {
		*failed_stage = "read";
		ret = ubi_volume_read((char *)source->volume, (char *)load_addr,
				      required);
		if (ret)
			goto out;
	}

	*loaded_size = required;

out:
	/* Also detach after a successful read followed by format fallback. */
	ubi_part_detach();
	return ret;
}

static int ipq_load_raw_kernel(const struct ipq_kernel_source *source,
		unsigned int load_addr, size_t *loaded_size,
		const char **failed_stage)
{
	nand_info_t *nand;
	loff_t offset;
	size_t part_size;
	size_t read_size;
	size_t actual;
	size_t required;
	int nand_dev;
	int ret;

	*failed_stage = "partition";
	ret = ipq_resolve_nand_partition(source->partition, &offset,
					 &part_size, &nand_dev);
	if (ret)
		return ret;

	nand = &nand_info[nand_dev];
	read_size = min_t(size_t, part_size, nand->erasesize);
	ret = ipq_kernel_memory_check(load_addr, read_size);
	if (ret)
		return ret;

	*failed_stage = "read";
	actual = 0;
	ret = nand_read_skip_bad(nand, offset, &read_size, &actual,
				 part_size, (u_char *)load_addr);
	if (ret)
		return ret;

	*failed_stage = "image-format";
	ret = ipq_image_required_size(load_addr, read_size, &required);
	if (ret)
		return ret;
	if (required > part_size)
		return -EFBIG;
	ret = ipq_kernel_memory_check(load_addr, required);
	if (ret)
		return ret;

	if (required > read_size) {
		read_size = required;
		actual = 0;
		*failed_stage = "read";
		ret = nand_read_skip_bad(nand, offset, &read_size, &actual,
					 part_size, (u_char *)load_addr);
		if (ret)
			return ret;
	}

	*loaded_size = required;
	return 0;
}

/*
 * Only source discovery, attach/read and structural image checks may advance
 * to the next entry.  Authentication, bootargs mutation and bootm deliberately
 * happen after this function returns and therefore can never trigger fallback.
 */
static int ipq_load_kernel_sources(unsigned int load_addr, char *runcmd,
		int runcmd_size, unsigned int *fit_addr)
{
	const struct ipq_kernel_source *source;
	const char *failed_stage;
	size_t loaded_size;
	int last_ret = -ENOENT;
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(ipq_kernel_sources); i++) {
		source = &ipq_kernel_sources[i];
		loaded_size = 0;
		failed_stage = "partition";
		printf("bootipq: try source=%s partition=%s",
		       ipq_kernel_source_name(source), source->partition);
		if (source->volume)
			printf(" volume=%s", source->volume);
		printf("\n");

		if (source->type == IPQ_KERNEL_UBI)
			ret = ipq_load_ubi_kernel(source, load_addr, &loaded_size,
						  &failed_stage);
		else
			ret = ipq_load_raw_kernel(source, load_addr, &loaded_size,
						  &failed_stage);
		if (!ret) {
			failed_stage = "image-format";
			ret = ipq_prepare_kernel_image(load_addr, loaded_size,
						       runcmd, runcmd_size,
						       fit_addr);
		}
		if (ret) {
			last_ret = ret;
			printf("bootipq: source=%s partition=%s stage=%s "
			       "errno=%d action=next\n",
			       ipq_kernel_source_name(source), source->partition,
			       failed_stage, ret);
			continue;
		}

		printf("bootipq: selected source=%s partition=%s",
		       ipq_kernel_source_name(source), source->partition);
		if (source->volume)
			printf(" volume=%s", source->volume);
		printf(" size=0x%zx\n", loaded_size);
		return 0;
	}

	printf("bootipq: all kernel sources failed last_errno=%d\n", last_ret);
	return last_ret;
}

static int ipq_use_nand_kernel_sources(void)
{
	return sfi->flash_type == SMEM_BOOT_NAND_FLASH ||
	       sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH ||
	       ipq_has_secondary_nand();
}
#endif

#ifndef CONFIG_DISABLE_SIGNED_BOOT
static int copy_rootfs(unsigned int request, uint32_t size)
{
	char runcmd[256];
#ifdef CONFIG_QCA_MMC
	int ret;
	block_dev_desc_t *blk_dev;
	disk_partition_t disk_info;
	unsigned int active_part = 0;
#endif

	if (ipq_fs_on_nand) {
		snprintf(runcmd, sizeof(runcmd),
			"ubi read 0x%x ubi_rootfs &&", request);
#ifdef CONFIG_QCA_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH ||
			((sfi->flash_type == SMEM_BOOT_SPI_FLASH) &&
			(sfi->rootfs.offset == 0xBAD0FF5E))) {
		blk_dev = mmc_get_dev(host->dev_num);
		if (smem_bootconfig_info() == 0) {
			active_part = get_rootfs_active_partition();
			if (active_part) {
				ret = get_partition_info_efi_by_name(blk_dev,
						"rootfs_1", &disk_info);
			} else {
				ret = get_partition_info_efi_by_name(blk_dev,
						"rootfs", &disk_info);
			}
		}else {
			ret = get_partition_info_efi_by_name(blk_dev,
					"rootfs", &disk_info);
		}
		if(ret == 0)
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%X 0x%X &&",
					request, (uint)disk_info.start,
					(uint)(size / disk_info.blksz) + 1);
		else
			return CMD_RET_FAILURE;
#endif
	} else {
		snprintf(runcmd, sizeof(runcmd),
			"sf read 0x%x 0x%x 0x%x && ",
			request, (uint)sfi->rootfs.offset, (uint)sfi->rootfs.size);
	}
	if (debug)
		printf("runcmd: %s\n", runcmd);
	if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
		return CMD_RET_FAILURE;

	return 0;
}

#ifndef CONFIG_IPQ_ELF_AUTH
static int authenticate_rootfs(unsigned int kernel_addr)
{
	unsigned int kernel_imgsize;
	unsigned int request;
	int ret;
	mbn_header_t *mbn_ptr;
	struct {
		unsigned long type;
		unsigned long size;
		unsigned long addr;
	} rootfs_img_info;

	request = CONFIG_ROOTFS_LOAD_ADDR;
	rootfs_img_info.addr = CONFIG_ROOTFS_LOAD_ADDR;
	rootfs_img_info.type = SEC_AUTH_SW_ID;
	request += sizeof(mbn_header_t);/* space for mbn header */

	/* get , kernel size = header + kernel + certificate */
	mbn_ptr = (mbn_header_t *) kernel_addr;
	kernel_imgsize = mbn_ptr->image_size + sizeof(mbn_header_t);

	/* get rootfs MBN header and validate it */
	mbn_ptr = (mbn_header_t *)((uint32_t)mbn_ptr + kernel_imgsize);
	if (mbn_ptr->image_type != ROOTFS_IMAGE_TYPE &&
			(mbn_ptr->code_size + mbn_ptr->signature_size +
			 mbn_ptr->cert_chain_size != mbn_ptr->image_size))
		return -EINVAL;

	/* pack, MBN header + rootfs + certificate */
	/* copy rootfs from the boot device */
	ret = copy_rootfs(request, mbn_ptr->code_size);
	if (ret)
		return ret;

	/* copy rootfs MBN header */
	memcpy((void *)CONFIG_ROOTFS_LOAD_ADDR, (void *)kernel_addr + kernel_imgsize,
			sizeof(mbn_header_t));
	/* copy rootfs certificate */
	memcpy((void *)request + mbn_ptr->code_size,
		(void *)kernel_addr + kernel_imgsize + sizeof(mbn_header_t),
		mbn_ptr->signature_size + mbn_ptr->cert_chain_size);

	/* copy rootfs size */
	rootfs_img_info.size = sizeof(mbn_header_t) + mbn_ptr->image_size;

	ret = qca_scm_secure_authenticate(&rootfs_img_info, sizeof(rootfs_img_info));

	memset((void *)kernel_img_info.kernel_load_addr,  0, sizeof(mbn_header_t));
	memset(mbn_ptr,  0,
		(sizeof(mbn_header_t) + mbn_ptr->signature_size + mbn_ptr->cert_chain_size));

	return ret;
}
#else

static int authenticate_rootfs_elf(unsigned int rootfs_hdr)
{
	int ret;
	unsigned int request;
	image_info img_info;
	struct {
		unsigned long type;
		unsigned long size;
		unsigned long addr;
	} rootfs_img_info;

	ret = parse_elf_image_phdr(&img_info, rootfs_hdr);
	if (ret)
		return ret;

	request = img_info.img_load_addr;
	memcpy((void*)request, (void*)rootfs_hdr, img_info.img_offset);

	/* copy rootfs from the boot device */
	ret = copy_rootfs(request + img_info.img_offset, img_info.img_size);
	if (ret)
		return ret;

	rootfs_img_info.addr = request;
	rootfs_img_info.type = SEC_AUTH_SW_ID;
	rootfs_img_info.size = img_info.img_offset + img_info.img_size;
	ret = qca_scm_secure_authenticate(&rootfs_img_info, sizeof(rootfs_img_info));
	memset((void *)rootfs_hdr, 0, img_info.img_offset);
	return ret;
}
#endif

static int do_boot_signedimg(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	char runcmd[256];
	char * const arg[1] = {runcmd};
	int ret;
	unsigned int request;
#ifdef CONFIG_VERSION_ROLLBACK_PARTITION_INFO
	int part = PRIMARY_PARTITION;
#endif
#ifdef CONFIG_QCA_MMC
	block_dev_desc_t *blk_dev;
	disk_partition_t disk_info;
	unsigned int active_part = 0;
#endif
#ifdef CONFIG_IPQ_ELF_AUTH
	image_info img_info;
#endif

	if (argc == 2 && strncmp(argv[1], "debug", 5) == 0)
		debug = 1;

	ret = set_fs_bootargs();
	if (ret)
		return ret;

	/* check the smem info to see which flash used for booting */
	if (sfi->flash_type == SMEM_BOOT_SPI_FLASH) {
		if (debug) {
			printf("Using nand device %d\n", CONFIG_SPI_FLASH_INFO_IDX);
		}
	} else if (((sfi->flash_type == SMEM_BOOT_NAND_FLASH) ||
		(sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH))) {
		if (debug) {
			printf("Using nand device 0\n");
		}
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH) {
		if (debug) {
			printf("Using MMC device\n");
		}
	} else {
		printf("Unsupported BOOT flash type\n");
		return -1;
	}
	if (debug) {
		printf("bootargs=%s\n", getenv("bootargs") ?
		       getenv("bootargs") : "<unset>");
		printf("Booting from flash\n");
	}

	request = CONFIG_SYS_LOAD_ADDR;
	kernel_img_info.kernel_load_addr = request;

#ifdef CONFIG_VERSION_ROLLBACK_PARTITION_INFO
	if (smem_bootconfig_info() == 0){
		ret = get_rootfs_active_partition();
		if (ret){
			part = SECONDARY_PARTITION;
		}
	}
	ret = qca_scm_part_info(&part, sizeof(part));
	if (ret) {
		printf("bootipq: fatal stage=auth target=partition-info "
		       "errno=%d action=stop\n", ret);
		BUG();
	}
#endif
	if (ipq_fs_on_nand) {
#ifdef CONFIG_CMD_UBI
		/*
		 * The kernel will be available inside a UBI volume
		 */
		snprintf(runcmd, sizeof(runcmd),
			 "nand device %d && "
			 "setenv mtdids nand%d=nand%d && "
			 "setenv mtdparts mtdparts=nand%d:0x%llx@0x%llx(fs),${msmparts} && "
			 "ubi part fs && ", is_spi_nand_available(),
			 is_spi_nand_available(),
			 is_spi_nand_available(),
			 is_spi_nand_available(),
			 sfi->rootfs.size, sfi->rootfs.offset);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

#ifdef CONFIG_IPQ_ELF_AUTH
		snprintf(runcmd, sizeof(runcmd),
			 "ubi read 0x%x kernel 0x%x && ",
			 request, ELF_HDR_PLUS_PHDR_SIZE);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (parse_elf_image_phdr(&img_info, request))
			return CMD_RET_FAILURE;

		request = img_info.img_load_addr - img_info.img_offset;
#endif
		snprintf(runcmd, sizeof(runcmd),
			 "ubi read 0x%x kernel && ", request);

		if (debug)
			printf("%s", runcmd);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		kernel_img_info.kernel_load_size =
			(unsigned int)ubi_get_volume_size("kernel");
#endif
#ifdef CONFIG_QCA_MMC
	} else if (sfi->flash_type == SMEM_BOOT_MMC_FLASH ||
			((sfi->flash_type == SMEM_BOOT_SPI_FLASH) &&
			(sfi->rootfs.offset == 0xBAD0FF5E))) {
		blk_dev = mmc_get_dev(host->dev_num);
		if (smem_bootconfig_info() == 0) {
			active_part = get_rootfs_active_partition();
			if (active_part) {
				ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS_1", &disk_info);
			} else {
				ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS", &disk_info);
			}
		} else {
			ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS", &disk_info);
		}

		if (ret == 0) {
#ifdef CONFIG_IPQ_ELF_AUTH
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%X 0x%X",
				 CONFIG_SYS_LOAD_ADDR,
				 (uint)disk_info.start, ELF_HDR_PLUS_PHDR_SIZE);

			if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
				return CMD_RET_FAILURE;

			if (parse_elf_image_phdr(&img_info, request))
				return CMD_RET_FAILURE;

			request = img_info.img_load_addr - img_info.img_offset;
#endif
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%X 0x%X",
				 request,
				 (uint)disk_info.start, (uint)disk_info.size);

			if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
				return CMD_RET_FAILURE;

			kernel_img_info.kernel_load_size = disk_info.size * disk_info.blksz;
		}
#endif
	} else {
		/*
		 * Kernel is in a separate partition
		 */
		snprintf(runcmd, sizeof(runcmd), "sf probe &&");

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

#ifdef CONFIG_IPQ_ELF_AUTH
		snprintf(runcmd, sizeof(runcmd),
			 "sf read 0x%x 0x%x 0x%x && ",
			 CONFIG_SYS_LOAD_ADDR,
			 (uint)sfi->hlos.offset, ELF_HDR_PLUS_PHDR_SIZE);

		if (debug)
			printf("%s", runcmd);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		if (parse_elf_image_phdr(&img_info, request))
			return CMD_RET_FAILURE;

		request = img_info.img_load_addr - img_info.img_offset;
#endif
		snprintf(runcmd, sizeof(runcmd),
			 "sf read 0x%x 0x%x 0x%x && ",
			 request,
			 (uint)sfi->hlos.offset, (uint)sfi->hlos.size);

		if (debug)
			printf("%s", runcmd);

		if (run_command(runcmd, 0) != CMD_RET_SUCCESS)
			return CMD_RET_FAILURE;

		kernel_img_info.kernel_load_size =  sfi->hlos.size;
	}

	setenv("mtdids", mtdids);

#ifndef CONFIG_IPQ_ELF_AUTH
	mbn_header_t * mbn_ptr = (mbn_header_t *) request;
	request += sizeof(mbn_header_t);
#else
	kernel_img_info.kernel_load_addr = request;
	request = img_info.img_load_addr;
#endif

	/* This sys call will switch the CE1 channel to register usage */
	ret = switch_ce_channel_buf(0);

	if (ret)
		return CMD_RET_FAILURE;

	ret = qca_scm_auth_kernel(&kernel_img_info,
			sizeof(kernel_img_info));
	if (ret) {
		printf("bootipq: fatal stage=auth target=kernel "
		       "errno=%d action=stop\n", ret);
		BUG();
	}
#ifndef CONFIG_IPQ_ELF_AUTH
	memset((void *)mbn_ptr->signature_ptr, 0,(mbn_ptr->signature_size + mbn_ptr->cert_chain_size));
#else
	memset((void *)kernel_img_info.kernel_load_addr,  0, img_info.img_offset);
#endif
	if (getenv("rootfs_auth")) {
#ifdef CONFIG_IPQ_ELF_AUTH
		ret = authenticate_rootfs_elf(img_info.img_load_addr +
					   img_info.img_size);
		if (ret != CMD_RET_SUCCESS) {
			printf("bootipq: fatal stage=auth target=rootfs-elf "
			       "errno=%d action=stop\n", ret);
			BUG();
		}
#else
		/* Rootfs's header and certificate at end of kernel image, copy from
		 * there and pack with rootfs image and authenticate rootfs */
		ret = authenticate_rootfs(CONFIG_SYS_LOAD_ADDR);
		if (ret != CMD_RET_SUCCESS) {
			printf("bootipq: fatal stage=auth target=rootfs "
			       "errno=%d action=stop\n", ret);
			BUG();
		}
#endif
	}

#ifdef CONFIG_SKIP_RESET
	if (apps_iscrashed())
		return 1;
#endif

	/*
	* This sys call will switch the CE1 channel to ADM usage
	* so that HLOS can use it.
	*/
	ret = switch_ce_channel_buf(1);

	if (ret)
		return CMD_RET_FAILURE;

	dcache_enable();

	ret = config_select(request, runcmd, sizeof(runcmd));

	if (debug)
		printf("%s", runcmd);

	if (ret < 0 || boot_os(1, arg) != CMD_RET_SUCCESS) {
#ifdef CONFIG_QCA_MMC
		mmc_initialize(gd->bd);
#endif
#ifdef CONFIG_USB_XHCI_IPQ
		ipq_board_usb_init();
#endif
		dcache_disable();
		return CMD_RET_FAILURE;
	}

#ifndef CONFIG_QCA_APPSBL_DLOAD
	reset_crashdump();
#endif
	return CMD_RET_SUCCESS;
}
#endif

static int do_boot_unsignedimg(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret;
	char runcmd[256];
	char * const arg[1] = {runcmd};
#if (defined(CONFIG_ARCH_IPQ6018) || defined(CONFIG_ARCH_IPQ807x)) && \
	defined(CONFIG_CMD_UBI) && defined(CONFIG_CMD_NAND)
	unsigned int fit_addr = 0;
#endif
#ifdef CONFIG_QCA_MMC
	block_dev_desc_t *blk_dev;
	disk_partition_t disk_info;
	unsigned int active_part = 0;
#endif
#ifdef CONFIG_IPQ_ELF_AUTH
	image_info img_info;
#endif

	if (argc == 2 && strncmp(argv[1], "debug", 5) == 0)
		debug = 1;

	ret = set_fs_bootargs();
	if (ret)
		return ret;

	if (debug) {
		printf("bootargs=%s\n", getenv("bootargs") ?
		       getenv("bootargs") : "<unset>");
		printf("Booting from flash\n");
	}

#if (defined(CONFIG_ARCH_IPQ6018) || defined(CONFIG_ARCH_IPQ807x)) && \
	defined(CONFIG_CMD_UBI) && defined(CONFIG_CMD_NAND)
	if (ipq_use_nand_kernel_sources()) {
		ret = ipq_load_kernel_sources(CONFIG_SYS_LOAD_ADDR, runcmd,
					      sizeof(runcmd), &fit_addr);
		if (ret)
			return CMD_RET_FAILURE;

		dcache_enable();
		setenv("mtdids", mtdids);
		if (fit_addr)
			ret = update_bootargs((void *)fit_addr);
		if (fit_addr && ret) {
			printf("bootipq: fatal source=selected stage=bootargs "
			       "errno=%d action=stop\n",
			       ret < 0 ? ret : -EINVAL);
			dcache_disable();
			return CMD_RET_FAILURE;
		}
		goto kernel_ready;
	}
#endif

	if (((sfi->flash_type == SMEM_BOOT_NAND_FLASH) ||
			(sfi->flash_type == SMEM_BOOT_QSPI_NAND_FLASH))) {
		if (debug) {
			printf("Using nand device 0\n");
		}

		/*
		 * The kernel is in seperate partition
		 */
		if (sfi->rootfs.offset == 0xBAD0FF5E) {
			printf(" bad offset of hlos");
			return -1;
		}

		snprintf(runcmd, sizeof(runcmd),
			 "setenv mtdids nand0=nand0 && "
			 "setenv mtdparts mtdparts=nand0:0x%llx@0x%llx(fs),${msmparts} && "
			 "ubi part fs && "
			 "ubi read 0x%x kernel && ",
			 sfi->rootfs.size, sfi->rootfs.offset,
			 CONFIG_SYS_LOAD_ADDR);

	} else if (((sfi->flash_type == SMEM_BOOT_SPI_FLASH) &&
		    (sfi->rootfs.offset != 0xBAD0FF5E)) ||
		   ipq_fs_on_nand) {
		if (get_which_flash_param("rootfs") || ipq_fs_on_nand) {
			snprintf(runcmd, sizeof(runcmd),
				 "nand device %d && "
				 "setenv mtdids nand%d=nand%d && "
				 "setenv mtdparts mtdparts=nand%d:0x%llx@0x%llx(fs),${msmparts} && "
				 "ubi part fs && "
				 "ubi read 0x%x kernel && ",
				 is_spi_nand_available(),
				 is_spi_nand_available(),
				 is_spi_nand_available(),
				 is_spi_nand_available(),
				 sfi->rootfs.size, sfi->rootfs.offset,
				 CONFIG_SYS_LOAD_ADDR);
		} else {
			/*
			 * Kernel is in a separate partition
			 */
			snprintf(runcmd, sizeof(runcmd),
				 "sf probe &&"
				 "sf read 0x%x 0x%x 0x%x && ",
				 CONFIG_SYS_LOAD_ADDR, (uint)sfi->hlos.offset, (uint)sfi->hlos.size);
		}
#ifdef CONFIG_QCA_MMC
	} else if ((sfi->flash_type == SMEM_BOOT_MMC_FLASH) ||
			((sfi->flash_type == SMEM_BOOT_SPI_FLASH) &&
			(sfi->rootfs.offset == 0xBAD0FF5E))) {
		if (debug) {
			printf("Using MMC device\n");
		}
		blk_dev = mmc_get_dev(host->dev_num);
		if (smem_bootconfig_info() == 0) {
			active_part = get_rootfs_active_partition();
			if (active_part) {
				ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS_1", &disk_info);
			} else {
				ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS", &disk_info);
			}
		} else {
			ret = get_partition_info_efi_by_name(blk_dev,
						"0:HLOS", &disk_info);
		}

		if (ret == 0) {
			snprintf(runcmd, sizeof(runcmd), "mmc read 0x%x 0x%x 0x%x",
				 CONFIG_SYS_LOAD_ADDR,
				 (uint)disk_info.start, (uint)disk_info.size);
		}

#endif   /* CONFIG_QCA_MMC   */
	} else {
		printf("Unsupported BOOT flash type\n");
		return -1;
	}

	if (run_command(runcmd, 0) != CMD_RET_SUCCESS) {
#ifdef CONFIG_QCA_MMC
		mmc_initialize(gd->bd);
#endif
		return CMD_RET_FAILURE;
	}

	dcache_enable();

	setenv("mtdids", mtdids);

	ret = genimg_get_format((void *)CONFIG_SYS_LOAD_ADDR);
	if (ret == IMAGE_FORMAT_FIT) {
		ret = config_select(CONFIG_SYS_LOAD_ADDR,
				    runcmd, sizeof(runcmd));
	} else if (ret == IMAGE_FORMAT_LEGACY) {
		snprintf(runcmd, sizeof(runcmd),
			 "0x%x", CONFIG_SYS_LOAD_ADDR);
	} else {
		ret = genimg_get_format((void *)CONFIG_SYS_LOAD_ADDR +
					sizeof(mbn_header_t));
		if (ret == IMAGE_FORMAT_FIT) {
			ret = config_select((CONFIG_SYS_LOAD_ADDR
					     + sizeof(mbn_header_t)),
					    runcmd, sizeof(runcmd));
#ifdef CONFIG_IPQ_ELF_AUTH
		} else if (!parse_elf_image_phdr(&img_info,
					CONFIG_SYS_LOAD_ADDR)) {
			ret = config_select((CONFIG_SYS_LOAD_ADDR +
						img_info.img_offset),
					    runcmd, sizeof(runcmd));
#endif
		} else if (ret == IMAGE_FORMAT_LEGACY) {
			snprintf(runcmd, sizeof(runcmd),
				 "0x%x", (CONFIG_SYS_LOAD_ADDR +
						  sizeof(mbn_header_t)));

		} else {
			dcache_disable();
			return CMD_RET_FAILURE;
		}
	}

#if (defined(CONFIG_ARCH_IPQ6018) || defined(CONFIG_ARCH_IPQ807x)) && \
	defined(CONFIG_CMD_UBI) && defined(CONFIG_CMD_NAND)
kernel_ready:
#endif
#ifdef CONFIG_SKIP_RESET
	if (apps_iscrashed())
		return 1;
#endif

	if (ret < 0) {
		printf("bootipq: fatal stage=image-format errno=%d "
		       "action=stop\n", ret);
	} else {
		ret = boot_os(1, arg);
		if (ret != CMD_RET_SUCCESS)
			printf("bootipq: fatal stage=bootm errno=%d "
			       "action=stop\n", ret);
	}

	if (ret != CMD_RET_SUCCESS) {
#ifdef CONFIG_USB_XHCI_IPQ
		ipq_board_usb_init();
#endif
		dcache_disable();
		return CMD_RET_FAILURE;
	}
#ifndef CONFIG_QCA_APPSBL_DLOAD
	reset_crashdump();
#endif
	return CMD_RET_SUCCESS;
}

static int do_bootipq(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret;
#ifndef CONFIG_DISABLE_SIGNED_BOOT
	char buf = 0;
#endif
	/*
	 * set fdt_high parameter so that u-boot will not load
	 * dtb above CONFIG_IPQ40XX_FDT_HIGH region.
	 */
	if (setenv("fdt_high", MK_STR(CONFIG_IPQ_FDT_HIGH))) {
		return CMD_RET_FAILURE;
	}

#if defined(CONFIG_IPQ9574_EDMA) || defined(CONFIG_IPQ5332_EDMA)
	aquantia_phy_reset_init_done();
#endif

#ifndef CONFIG_DISABLE_SIGNED_BOOT
	ret = qca_scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD, &buf, sizeof(char));

	/*
	|| if atf is enable in env ,do_boot_signedimg is skip.
	|| Note: This features currently support in ipq50XX.
	*/
	if (ret == 0 && buf == 1 && !is_atf_enabled()) {
		ret = do_boot_signedimg(cmdtp, flag, argc, argv);
	} else if (ret == 0 || ret == -EOPNOTSUPP) {
		ret = do_boot_unsignedimg(cmdtp, flag, argc, argv);
	} else {
		printf("bootipq: fatal stage=secure-state errno=%d action=stop\n",
		       ret);
		ret = CMD_RET_FAILURE;
	}
#else
	ret = do_boot_unsignedimg(cmdtp, flag, argc, argv);
#endif

	if (ret == CMD_RET_FAILURE) {
#ifdef CONFIG_HTTPD
	puts("bootipq failed, enter web failsafe mode\n");
	run_command("httpd", 0);
	cli_loop();
#else
#if !defined(CONFIG_IPQ5332) && !defined(CONFIG_IPQ9574)
#ifdef CONFIG_IPQ_ETH_INIT_DEFER
		puts("\nNet:   ");
		eth_initialize();
#endif /* CONFIG_IPQ_ETH_INIT_DEFER */
#endif /* CONFIG_IPQ5332, CONFIG_IPQ9574 */
#endif /* CONFIG_HTTPD */
	}

	return ret;
}

U_BOOT_CMD(bootipq, 2, 0, do_bootipq,
	   "bootipq from flash device",
	   "bootipq [debug] - Load image(s) and boots the kernel\n");
