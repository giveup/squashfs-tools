/*
 * Create a squashfs filesystem.  This is a highly compressed read only
 * filesystem.
 *
 * Copyright (c) 2009, 2010, 2012, 2014
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * pseudo.c
 */

#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

#include "pseudo.h"
#include "error.h"
#include "progressbar.h"

#define TRUE 1
#define FALSE 0

extern int read_file(char *filename, char *type, int (parse_line)(char *));

struct pseudo_dev **pseudo_file = NULL;
struct pseudo *pseudo = NULL;
int pseudo_count = 0;

static void dump_pseudo(struct pseudo *pseudo, char *string)
{
	int i, res;
	char *path;

	for(i = 0; i < pseudo->names; i++) {
		struct pseudo_entry *entry = &pseudo->name[i];
		if(string) {
			res = asprintf(&path, "%s/%s", string, entry->name);
			if(res == -1)
				BAD_ERROR("asprintf failed in dump_pseudo\n");
		} else
			path = entry->name;
		if(entry->pseudo == NULL)
			ERROR("%s %c %o %d %d %d %d\n", path, entry->dev->type,
				entry->dev->mode, entry->dev->uid,
				entry->dev->gid, entry->dev->major,
				entry->dev->minor);
		else
			dump_pseudo(entry->pseudo, path);
		if(string)
			free(path);
	}
}


static char *get_component(char *target, char **targname)
{
	char *start;

	while(*target == '/')
		target ++;

	start = target;
	while(*target != '/' && *target!= '\0')
		target ++;

	*targname = strndup(start, target - start);

	return target;
}


/*
 * Add pseudo device target to the set of pseudo devices.  Pseudo_dev
 * describes the pseudo device attributes.
 */
struct pseudo *add_pseudo(struct pseudo *pseudo, struct pseudo_dev *pseudo_dev,
	char *target, char *alltarget)
{
	char *targname;
	int i;

	target = get_component(target, &targname);

	if(pseudo == NULL) {
		pseudo = malloc(sizeof(struct pseudo));
		if(pseudo == NULL)
			MEM_ERROR();

		pseudo->names = 0;
		pseudo->count = 0;
		pseudo->name = NULL;
	}

	for(i = 0; i < pseudo->names; i++)
		if(strcmp(pseudo->name[i].name, targname) == 0)
			break;

	if(i == pseudo->names) {
		/* allocate new name entry */
		pseudo->names ++;
		pseudo->name = realloc(pseudo->name, (i + 1) *
			sizeof(struct pseudo_entry));
		if(pseudo->name == NULL)
			MEM_ERROR();
		pseudo->name[i].name = targname;

		if(target[0] == '\0') {
			/* at leaf pathname component */
			pseudo->name[i].pseudo = NULL;
			pseudo->name[i].pathname = strdup(alltarget);
			pseudo->name[i].dev = pseudo_dev;
		} else {
			/* recurse adding child components */
			pseudo->name[i].dev = NULL;
			pseudo->name[i].pseudo = add_pseudo(NULL, pseudo_dev,
				target, alltarget);
		}
	} else {
		/* existing matching entry */
		free(targname);

		if(pseudo->name[i].pseudo == NULL) {
			/* No sub-directory which means this is the leaf
			 * component of a pre-existing pseudo file.
			 */
			if(target[0] != '\0') {
				/*
				 * entry must exist as either a 'd' type or
				 * 'm' type pseudo file
				 */
				if(pseudo->name[i].dev->type == 'd' ||
					pseudo->name[i].dev->type == 'm')
					/* recurse adding child components */
					pseudo->name[i].pseudo =
						add_pseudo(NULL, pseudo_dev,
						target, alltarget);
				else {
					ERROR_START("%s already exists as a "
						"non directory.", targname);
					ERROR_EXIT(".  Ignoring %s!\n",
						alltarget);
				}
			} else if(memcmp(pseudo_dev, pseudo->name[i].dev,
					sizeof(struct pseudo_dev)) != 0) {
				ERROR_START("%s already exists as a different "
					"pseudo definition.", alltarget);
				ERROR_EXIT("  Ignoring!\n");
			} else {
				ERROR_START("%s already exists as an identical "
					"pseudo definition!", alltarget);
				ERROR_EXIT("  Ignoring!\n");
			}
		} else {
			if(target[0] == '\0') {
				/*
				 * sub-directory exists, which means we can only
				 * add a pseudo file of type 'd' or type 'm'
				 */
				if(pseudo->name[i].dev == NULL &&
						(pseudo_dev->type == 'd' ||
						pseudo_dev->type == 'm')) {
					pseudo->name[i].pathname =
						strdup(alltarget);
					pseudo->name[i].dev = pseudo_dev;
				} else {
					ERROR_START("%s already exists as a "
						"different pseudo definition.",
						targname);
					ERROR_EXIT("  Ignoring %s!\n",
						alltarget);
				}
			} else
				/* recurse adding child components */
				add_pseudo(pseudo->name[i].pseudo, pseudo_dev,
					target, alltarget);
		}
	}

	return pseudo;
}


/*
 * Find subdirectory in pseudo directory referenced by pseudo, matching
 * filename.  If filename doesn't exist or if filename is a leaf file
 * return NULL
 */
struct pseudo *pseudo_subdir(char *filename, struct pseudo *pseudo)
{
	int i;

	if(pseudo == NULL)
		return NULL;

	for(i = 0; i < pseudo->names; i++)
		if(strcmp(filename, pseudo->name[i].name) == 0)
			return pseudo->name[i].pseudo;

	return NULL;
}


struct pseudo_entry *pseudo_readdir(struct pseudo *pseudo)
{
	if(pseudo == NULL)
		return NULL;

	while(pseudo->count < pseudo->names) {
		if(pseudo->name[pseudo->count].dev != NULL)
			return &pseudo->name[pseudo->count++];
		else
			pseudo->count++;
	}

	return NULL;
}


int exec_file(char *command, struct pseudo_dev *dev)
{
	int child, res;
	static pid_t pid = -1;
	int pipefd[2];
#ifdef USE_TMP_FILE
	char *filename;
	int status;
	static int number = 0;
#endif

	if(pid == -1)
		pid = getpid();

#ifdef USE_TMP_FILE
	res = asprintf(&filename, "/tmp/squashfs_pseudo_%d_%d", pid, number ++);
	if(res == -1) {
		ERROR("asprint failed in exec_file()\n");
		return -1;
	}
	pipefd[1] = open(filename, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);
	if(pipefd[1] == -1) {
		ERROR("Executing dynamic pseudo file, open failed\n");
		free(filename);
		return -1;
	}
#else
	res = pipe(pipefd);
	if(res == -1) {
		ERROR("Executing dynamic pseudo file, pipe failed\n");
		return -1;
	}
#endif

	child = fork();
	if(child == -1) {
		ERROR("Executing dynamic pseudo file, fork failed\n");
		goto failed;
	}

	if(child == 0) {
		close(STDOUT_FILENO);
		res = dup(pipefd[1]);
		if(res == -1)
			exit(EXIT_FAILURE);

		execl("/bin/sh", "sh", "-c", command, (char *) NULL);
		exit(EXIT_FAILURE);
	}

#ifdef USE_TMP_FILE
	res = waitpid(child, &status, 0);
	close(pipefd[1]);
	if(res != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		dev->filename = filename;
		return 0;
	}
failed:
	unlink(filename);
	free(filename);
	return -1;
#else
	close(pipefd[1]);
	dev->fd = pipefd[0];
	dev->child = child;
	return 0;
failed:
	return -1;
#endif
}


void add_pseudo_file(struct pseudo_dev *dev)
{
	pseudo_file = realloc(pseudo_file, (pseudo_count + 1) *
		sizeof(struct pseudo_dev *));
	if(pseudo_file == NULL)
		MEM_ERROR();

	dev->pseudo_id = pseudo_count;
	pseudo_file[pseudo_count ++] = dev;
}


void delete_pseudo_files()
{
#ifdef USE_TMP_FILE
	int i;

	for(i = 0; i < pseudo_count; i++)
		unlink(pseudo_file[i]->filename);
#endif
}


struct pseudo_dev *get_pseudo_file(int pseudo_id)
{
	return pseudo_file[pseudo_id];
}


int read_pseudo_def(char *def)
{
	int n, bytes;
	unsigned int major = 0, minor = 0, mode;
	char type, *ptr;
	char suid[100], sgid[100]; /* overflow safe */
	char *filename, *name;
	char *orig_def = def;
	long long uid, gid;
	struct pseudo_dev *dev;

	/*
	 * Scan for filename, don't use sscanf() and "%s" because
	 * that can't handle filenames with spaces
	 */
	filename = malloc(strlen(def) + 1);
	if(filename == NULL)
		MEM_ERROR();

	for(name = filename; !isspace(*def) && *def != '\0';) {
		if(*def == '\\') {
			def ++;
			if (*def == '\0')
				break;
		}
		*name ++ = *def ++;
	}
	*name = '\0';

	if(*filename == '\0') {
		ERROR("Not enough or invalid arguments in pseudo file "
			"definition \"%s\"\n", orig_def);
		goto error;
	}

	n = sscanf(def, " %c %o %99s %99s %n", &type, &mode, suid, sgid,
		&bytes);
	def += bytes;

	if(n < 4) {
		ERROR("Not enough or invalid arguments in pseudo file "
			"definition \"%s\"\n", orig_def);
		switch(n) {
		case -1:
			/* FALLTHROUGH */
		case 0:
			ERROR("Read filename, but failed to read or match "
				"type\n");
			break;
		case 1:
			ERROR("Read filename and type, but failed to read or "
				"match octal mode\n");
			break;
		case 2:
			ERROR("Read filename, type and mode, but failed to "
				"read or match uid\n");
			break;
		default:
			ERROR("Read filename, type, mode and uid, but failed "
				"to read or match gid\n");
			break; 
		}
		goto error;
	}

	switch(type) {
	case 'b':
		/* FALLTHROUGH */
	case 'c':
		n = sscanf(def, "%u %u %n", &major, &minor, &bytes);
		def += bytes;

		if(n < 2) {
			ERROR("Not enough or invalid arguments in %s device "
				"pseudo file definition \"%s\"\n", type == 'b' ?
				"block" : "character", orig_def);
			if(n < 1)
				ERROR("Read filename, type, mode, uid and gid, "
					"but failed to read or match major\n");
			else
				ERROR("Read filename, type, mode, uid, gid "
					"and major, but failed to read  or "
					"match minor\n");
			goto error;
		}	
		
		if(major > 0xfff) {
			ERROR("Major %d out of range\n", major);
			goto error;
		}

		if(minor > 0xfffff) {
			ERROR("Minor %d out of range\n", minor);
			goto error;
		}
		/* FALLTHROUGH */
	case 'd':
		/* FALLTHROUGH */
	case 'm':
		/*
		 * Check for trailing junk after expected arguments
		 */
		if(def[0] != '\0') {
			ERROR("Unexpected tailing characters in pseudo file "
				"definition \"%s\"\n", orig_def);
			goto error;
		}
		break;
	case 'f':
		if(def[0] == '\0') {
			ERROR("Not enough arguments in dynamic file pseudo "
				"definition \"%s\"\n", orig_def);
			ERROR("Expected command, which can be an executable "
				"or a piece of shell script\n");
			goto error;
		}	
		break;
	default:
		ERROR("Unsupported type %c\n", type);
		goto error;
	}


	if(mode > 07777) {
		ERROR("Mode %o out of range\n", mode);
		goto error;
	}

	uid = strtoll(suid, &ptr, 10);
	if(*ptr == '\0') {
		if(uid < 0 || uid > ((1LL << 32) - 1)) {
			ERROR("Uid %s out of range\n", suid);
			goto error;
		}
	} else {
		struct passwd *pwuid = getpwnam(suid);
		if(pwuid)
			uid = pwuid->pw_uid;
		else {
			ERROR("Uid %s invalid uid or unknown user\n", suid);
			goto error;
		}
	}
		
	gid = strtoll(sgid, &ptr, 10);
	if(*ptr == '\0') {
		if(gid < 0 || gid > ((1LL << 32) - 1)) {
			ERROR("Gid %s out of range\n", sgid);
			goto error;
		}
	} else {
		struct group *grgid = getgrnam(sgid);
		if(grgid)
			gid = grgid->gr_gid;
		else {
			ERROR("Gid %s invalid uid or unknown user\n", sgid);
			goto error;
		}
	}

	switch(type) {
	case 'b':
		mode |= S_IFBLK;
		break;
	case 'c':
		mode |= S_IFCHR;
		break;
	case 'd':
		mode |= S_IFDIR;
		break;
	case 'f':
		mode |= S_IFREG;
		break;
	}

	dev = malloc(sizeof(struct pseudo_dev));
	if(dev == NULL)
		MEM_ERROR();

	dev->type = type;
	dev->mode = mode;
	dev->uid = uid;
	dev->gid = gid;
	dev->major = major;
	dev->minor = minor;

	if(type == 'f') {
		int res;

		printf("Executing dynamic pseudo file\n");
		printf("\t\"%s\"\n", orig_def);
		res = exec_file(def, dev);
		if(res == -1) {
			ERROR("Failed to execute dynamic pseudo file definition"
				" \"%s\"\n", orig_def);
			free(filename);
			free(dev);
			return FALSE;
		}
		add_pseudo_file(dev);
	}

	pseudo = add_pseudo(pseudo, dev, filename, filename);

	free(filename);
	return TRUE;

error:
	ERROR("Pseudo definitions should be of format\n");
	ERROR("\tfilename d mode uid gid\n");
	ERROR("\tfilename m mode uid gid\n");
	ERROR("\tfilename b mode uid gid major minor\n");
	ERROR("\tfilename c mode uid gid major minor\n");
	ERROR("\tfilename f mode uid command\n");
	free(filename);
	return FALSE;
}


int read_pseudo_file(char *filename)
{
	return read_file(filename, "pseudo", read_pseudo_def);
}


struct pseudo *get_pseudo()
{
	return pseudo;
}
