/*
 * Copyright (C) 2024      Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-out-of-memory.h"
#include "core-put.h"

static const stress_help_t help[] = {
	{ NULL,	"mmapfiles N",		"start N workers stressing many mmaps and munmaps" },
	{ NULL,	"mmapfiles-ops N",	"stop after N mmapfiles bogo operations" },
	{ NULL, "mmapfiles-populate",	"populate memory mappings" },
	{ NULL, "mmapfiles-shared",	"enable shared mappings instead of private mappings" },
	{ NULL,	NULL,		  	NULL }
};

#define MMAP_MAX	(512 * 1024)

typedef struct {
	void *addr;
	size_t len;
} stress_mapping_t;

typedef struct {
	double mmap_page_count;
	double mmap_count;
	double mmap_duration;
	double munmap_page_count;
	double munmap_count;
	double munmap_duration;
} stress_mmapfile_info_t;

/*
 *  stress_set_mmapfiles_populate()
 *      set mmapfiles_populate flag
 */
static int stress_set_mmapfiles_populate(const char *opt)
{
        return stress_set_setting_true("mmapfiles-populate", opt);
}

/*
 *  stress_set_mmapfiles_shared()
 *      set mmapfiles_shared flag
 */
static int stress_set_mmapfiles_shared(const char *opt)
{
        return stress_set_setting_true("mmapfiles-shared", opt);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_mmapfiles_populate,	stress_set_mmapfiles_populate },
	{ OPT_mmapfiles_shared,		stress_set_mmapfiles_shared },
	{ 0, 				NULL },
};

static size_t stress_mmapfiles_dir(
	stress_args_t *args,
	stress_mmapfile_info_t *mmapfile_info,
	const char *path,
	stress_mapping_t *mappings,
	size_t n_mappings,
	const bool mmap_populate,
	const bool mmap_shared,
	bool *enomem)
{
	DIR *dir;
	struct dirent *d;
	int flags = 0;

	flags |= mmap_shared ? MAP_SHARED : MAP_PRIVATE;
#if defined(MAP_POPULATE)
	flags |= mmap_populate ? MAP_POPULATE : 0;
#endif
	dir = opendir(path);
	if (!dir)
		return n_mappings;

	while (!(*enomem) && ((d = readdir(dir)) != NULL)) {
		if (n_mappings >= MMAP_MAX)
			break;
		if (!stress_continue_flag())
			break;
		if (stress_is_dot_filename(d->d_name))
			continue;
		if (d->d_type == DT_DIR) {
			char newpath[PATH_MAX];

			(void)snprintf(newpath, sizeof(newpath), "%s/%s", path, d->d_name);
			n_mappings = stress_mmapfiles_dir(args, mmapfile_info, newpath, mappings, n_mappings,
							mmap_populate, mmap_shared, enomem);
		} else if (d->d_type == DT_REG) {
			char filename[PATH_MAX];
			uint8_t *ptr;
			struct stat statbuf;
			int fd;
			double t, delta;
			size_t len;
			const size_t page_size = args->page_size;

			(void)snprintf(filename, sizeof(filename), "%s/%s", path, d->d_name);
			fd = open(filename, O_RDONLY);
			if (fd < 0)
				continue;

			if (fstat(fd, &statbuf) < 0) {
				(void)close(fd);
				continue;
			}
			len = statbuf.st_size;
			if ((g_opt_flags & OPT_FLAGS_OOM_AVOID) && stress_low_memory(len)) {
				(void)close(fd);
				break;
			}

			t = stress_time_now();
			ptr = (uint8_t *)mmap(NULL, len, PROT_READ, flags, fd, 0);
			delta = stress_time_now() - t;
			if (ptr != MAP_FAILED) {
				if (mmap_populate) {
					register size_t i;

					for (i = 0; i < len; i += page_size) {
						stress_uint8_put(*(ptr + i));
					}
				}
				mappings[n_mappings].addr = (void *)ptr;
				mappings[n_mappings].len = len;
				n_mappings++;
				mmapfile_info->mmap_count += 1.0;
				mmapfile_info->mmap_duration += delta;
				mmapfile_info->mmap_page_count += (double)(len + page_size - 1) / page_size;
				stress_bogo_inc(args);
			} else {
				if (errno == ENOMEM) {
					*enomem = true;
					(void)close(fd);
					break;
				}
			}
			(void)close(fd);
		}
	}
	(void)closedir(dir);
	return n_mappings;
}

static int stress_mmapfiles_child(stress_args_t *args, void *context)
{
	size_t idx = 0;
	stress_mmapfile_info_t *mmapfile_info = (stress_mmapfile_info_t *)context;
	stress_mapping_t *mappings;
	static const char *dirs[] = {
		"/lib",
		"/lib32",
		"/lib64",
		"/boot",
		"/bin",
		"/etc,",
		"/sbin",
		"/usr",
		"/var",
		"/sys",
		"/proc",
	};
	bool mmap_populate = false;
	bool mmap_shared = false;

	(void)stress_get_setting("mmapfiles-populate", &mmap_populate);
	(void)stress_get_setting("mmapfiles-shared", &mmap_shared);

	mappings = calloc((size_t)MMAP_MAX, sizeof(*mappings));
	if (!mappings) {
		pr_fail("%s: malloc failed, out of memory\n", args->name);
		return EXIT_NO_RESOURCE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		size_t i, n;

		for (n = 0, i = 0; i < SIZEOF_ARRAY(dirs); i++) {
			bool enomem = false;

			n = stress_mmapfiles_dir(args, mmapfile_info, dirs[idx], mappings, n,
						mmap_populate, mmap_shared, &enomem);
			idx++;
			if (idx >= SIZEOF_ARRAY(dirs))
				idx = 0;
			if (enomem)
				break;
		}

		for (i = 0; i < n; i++) {
			double t, delta;
			const size_t len = mappings[i].len;

			t = stress_time_now();
			if (munmap((void *)mappings[i].addr, len) == 0) {
				delta = stress_time_now() - t;
				mmapfile_info->munmap_duration += delta;
				mmapfile_info->munmap_count += 1.0;
				mmapfile_info->munmap_page_count += (double)(len + args->page_size - 1) / args->page_size;
			} else {
				(void)stress_munmap_retry_enomem((void *)mappings[i].addr, mappings[i].len);
			}
			mappings[i].addr = NULL;
			mappings[i].len = 0;
		}
	} while (stress_continue(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	free(mappings);
	return EXIT_SUCCESS;
}

/*
 *  stress_mmapfiles()
 *	stress mmap with many pages being mapped
 */
static int stress_mmapfiles(stress_args_t *args)
{
	stress_mmapfile_info_t *mmapfile_info;
	int ret;
	double metric;

	mmapfile_info = (stress_mmapfile_info_t *)stress_mmap_populate(NULL, sizeof(*mmapfile_info),
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS,
				-1, 0);
	if (mmapfile_info == MAP_FAILED) {
		pr_inf("%s: cannot mmap mmap file information, "
			"errno=%d (%s), skipping stressor\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	mmapfile_info->mmap_page_count = 0.0;
	mmapfile_info->mmap_count = 0.0;
	mmapfile_info->mmap_duration = 0.0;
	mmapfile_info->munmap_page_count = 0.0;
	mmapfile_info->munmap_count = 0.0;
	mmapfile_info->munmap_duration = 0.0;

	ret = stress_oomable_child(args, (void *)mmapfile_info, stress_mmapfiles_child, STRESS_OOMABLE_NORMAL);

	metric = (mmapfile_info->mmap_duration > 0.0) ? mmapfile_info->mmap_count / mmapfile_info->mmap_duration : 0.0;
	stress_metrics_set(args, 0, "file mmaps per sec ", metric, STRESS_HARMONIC_MEAN);
	metric = (mmapfile_info->munmap_duration > 0.0) ? mmapfile_info->munmap_count / mmapfile_info->munmap_duration : 0.0;
	stress_metrics_set(args, 1, "file munmap per sec", metric, STRESS_HARMONIC_MEAN);

	metric = (mmapfile_info->mmap_duration > 0.0) ? mmapfile_info->mmap_page_count / mmapfile_info->mmap_duration: 0.0;
	stress_metrics_set(args, 2, "file pages mmap'd per sec", metric, STRESS_HARMONIC_MEAN);
	metric = (mmapfile_info->munmap_duration > 0.0) ? mmapfile_info->munmap_page_count / mmapfile_info->munmap_duration: 0.0;
	stress_metrics_set(args, 3, "file pages munmap'd per sec", metric, STRESS_HARMONIC_MEAN);
	metric = (mmapfile_info->mmap_count > 0.0) ? mmapfile_info->mmap_page_count / mmapfile_info->mmap_count : 0.0;
	stress_metrics_set(args, 4, "pages per mapping", metric, STRESS_HARMONIC_MEAN);

	(void)munmap((void *)mmapfile_info, sizeof(*mmapfile_info));

	return ret;
}

stressor_info_t stress_mmapfiles_info = {
	.stressor = stress_mmapfiles,
	.class = CLASS_VM | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
