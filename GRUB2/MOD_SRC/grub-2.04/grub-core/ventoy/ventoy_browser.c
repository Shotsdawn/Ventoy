/******************************************************************************
 * ventoy_browser.c 
 *
 * Copyright (c) 2022, longpanda <admin@ventoy.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/device.h>
#include <grub/term.h>
#include <grub/partition.h>
#include <grub/file.h>
#include <grub/normal.h>
#include <grub/extcmd.h>
#include <grub/datetime.h>
#include <grub/i18n.h>
#include <grub/net.h>
#include <grub/time.h>
#include <grub/ventoy.h>
#include "ventoy_def.h"

GRUB_MOD_LICENSE ("GPLv3+");

#define BROWSER_MENU_BUF    65536

static grub_fs_t g_menu_fs = NULL;
static char *g_menu_device = NULL;
static char g_menu_path_buf[1024];
static int g_menu_path_len = 0;
static browser_node *g_browser_list = NULL;

static int ventoy_browser_strcmp(char *str1, char *str2)
{
    char *s1, *s2;
    int c1 = 0;
    int c2 = 0;

    for (s1 = str1, s2 = str2; *s1 && *s2; s1++, s2++)
    {
        c1 = *s1;
        c2 = *s2;

        if (0 == g_sort_case_sensitive)
        {
            if (grub_islower(c1))
            {
                c1 = c1 - 'a' + 'A';
            }
            
            if (grub_islower(c2))
            {
                c2 = c2 - 'a' + 'A';
            }
        }

        if (c1 != c2)
        {
            break;
        }
    }

    return (c1 - c2);
}

static int ventoy_browser_mbuf_alloc(browser_mbuf *mbuf)
{
    grub_memset(mbuf, 0, sizeof(browser_mbuf));
    mbuf->buf = grub_malloc(BROWSER_MENU_BUF);
    if (!mbuf->buf)
    {
        return 0;
    }

    mbuf->pos = 0;
    mbuf->max = BROWSER_MENU_BUF;
    return 1;
}

static void ventoy_browser_mbuf_free(browser_mbuf *mbuf)
{
    if (mbuf)
        grub_check_free(mbuf->buf)
}

static int ventoy_browser_mbuf_extend(browser_mbuf *mbuf)
{
    if (mbuf->max - mbuf->pos <= VTOY_SIZE_1KB)
    {
        mbuf->max += BROWSER_MENU_BUF;
        mbuf->buf = grub_realloc(mbuf->buf, mbuf->max);
    }

    return 0;
}

static browser_node * ventoy_browser_find_top_node(int dir)
{
    browser_node *node = NULL;
    browser_node *sel = NULL;

    for (node = g_browser_list; node; node = node->next)
    {
        if (node->dir == dir)
        {
            if (sel)
            {
                if (ventoy_browser_strcmp(sel->filename, node->filename) > 0)
                {
                    sel = node;
                }
            }
            else
            {
                sel = node;
            }
        }
    }
    
    return sel;
}

static int ventoy_browser_check_filename(const char *filename, int len, int *type)
{
    if (len < 4)
    {
        return 0;
    }

    if (FILE_FLT(ISO) && 0 == grub_strcasecmp(filename + len - 4, ".iso"))
    {
        *type = img_type_iso;
    }
    else if (FILE_FLT(WIM) && g_wimboot_enable && (0 == grub_strcasecmp(filename + len - 4, ".wim")))
    {
        *type = img_type_wim;
    }
    else if (FILE_FLT(VHD) && g_vhdboot_enable && (0 == grub_strcasecmp(filename + len - 4, ".vhd") || 
            (len >= 5 && 0 == grub_strcasecmp(filename + len - 5, ".vhdx"))))
    {
        *type = img_type_vhd;
    }
    #ifdef GRUB_MACHINE_EFI
    else if (FILE_FLT(EFI) && 0 == grub_strcasecmp(filename + len - 4, ".efi"))
    {
        *type = img_type_efi;
    }
    #endif
    else if (FILE_FLT(IMG) && 0 == grub_strcasecmp(filename + len - 4, ".img"))
    {
        if (len == 18 && grub_strncmp(filename, "ventoy_", 7) == 0)
        {
            if (grub_strncmp(filename + 7, "wimboot", 7) == 0 ||
                grub_strncmp(filename + 7, "vhdboot", 7) == 0)
            {
                return 0;
            }
        }
        *type = img_type_img;
    }
    else if (FILE_FLT(VTOY) && len >= 5 && 0 == grub_strcasecmp(filename + len - 5, ".vtoy"))
    {
        *type = img_type_vtoy;
    }
    else
    {
        return 0;
    }

    if (g_filt_dot_underscore_file && filename[0] == '.' && filename[1] == '_')
    {
        return 0;
    }

    return 1;
}


static int ventoy_browser_iterate_partition(struct grub_disk *disk, const grub_partition_t partition, void *data)
{
    char partname[64];
    char title[256];
    grub_device_t dev;
    grub_fs_t fs;
    char *Label = NULL;
    browser_mbuf *mbuf = (browser_mbuf *)data;

    (void)data;
    
    grub_snprintf(partname, sizeof(partname) - 1, "%s,%d", disk->name, partition->number + 1);

    dev = grub_device_open(partname);
    if (!dev)
    {
        return 0;
    }

    fs = grub_fs_probe(dev);
    if (!fs)
    {
        grub_device_close(dev);
        return 0;
    }

    fs->fs_label(dev, &Label);

    grub_snprintf(title, sizeof(title), "%-10s (%s,%s%d) [%s] %s %s", 
        "DISK", disk->name, partition->msdostype == 0xee ? "gpt" : "msdos", 
        partition->number + 1, (Label ? Label : ""), fs->name, 
        grub_get_human_size(partition->len << disk->log_sector_size, GRUB_HUMAN_SIZE_SHORT));

    if (ventoy_get_fs_type(fs->name) >= ventoy_fs_max)
    {
        browser_ssprintf(mbuf, "menuentry \"%s\" --class=vtoydisk {\n"
            "   echo \"unsupported file system type!\" \n"
            "   ventoy_pause\n"
            "}\n",
            title);
    }
    else
    {
        browser_ssprintf(mbuf, "menuentry \"%s\" --class=vtoydisk {\n"
            "  vt_browser_dir %s,%d 0x%lx /\n"
            "}\n",
            title, disk->name, partition->number + 1, (ulong)fs);
    }

    ventoy_browser_mbuf_extend(mbuf);

    return 0;
}


static int ventoy_browser_iterate_disk(const char *name, void *data)
{
    grub_disk_t disk;
    grub_uint32_t sig;
    grub_uint32_t selfsig;

    grub_memcpy(&selfsig, g_ventoy_part_info->MBR.BootCode + 0x1b8, 4);

    if (name[0] != 'h')
    {
        return 0;
    }

    disk = grub_disk_open(name);
    if (disk)
    {
        grub_disk_read(disk, 0, 0x1b8, 4, &sig);

        /* skip ventoy device self */
        if (sig != selfsig)
        {
            grub_partition_iterate(disk, ventoy_browser_iterate_partition, data);
        }
        
        grub_disk_close(disk);
    }

    return 0;
}

static int ventoy_browser_iterate_dir(const char *filename, const struct grub_dirhook_info *info, void *data)
{
    int type;
    int len;
    browser_node *node;

    (void)data;

    len = grub_strlen(filename);
    
    if (info->dir)
    {
        if ((len == 1 && filename[0] == '.') ||
            (len == 2 && filename[0] == '.' && filename[1] == '.'))
        {
            return 0;
        }

        if (!ventoy_img_name_valid(filename, len))
        {
            return 0;
        }

        if (filename[0] == '$')
        {
            if (0 == grub_strncmp(filename, "$RECYCLE.BIN", 12) ||
                0 == grub_strncasecmp(filename, "$Extend", 7))
            {
                return 0;
            }
        }

        node = grub_zalloc(sizeof(browser_node));
        if (!node)
        {
            return 0;
        }

        node->dir = 1;
        grub_strncpy(node->filename, filename, sizeof(node->filename));
        grub_snprintf(node->menuentry, sizeof(node->menuentry),
            "menuentry \"%-10s [%s]\" --class=vtoydir {\n"
            "  vt_browser_dir %s 0x%lx \"%s/%s\"\n"
            "}\n",
            "DIR", filename, g_menu_device, (ulong)g_menu_fs, g_menu_path_buf, filename);
    }
    else
    {
        grub_uint64_t fsize;
        grub_file_t file = NULL;
        
        if (ventoy_browser_check_filename(filename, len, &type) == 0)
        {
            return 0;
        }

        fsize = info->size;
        if (fsize == 0)
        {
            file = ventoy_grub_file_open(VENTOY_FILE_TYPE, "(%s)%s/%s", g_menu_device, g_menu_path_buf, filename);
            if (!file)
            {
                return 0;
            }

            fsize = file->size;
            grub_file_close(file);
        }

        if (fsize < VTOY_FILT_MIN_FILE_SIZE)
        {
            return 0;
        }

        node = grub_zalloc(sizeof(browser_node));
        if (!node)
        {
            return 0;
        }

        node->dir = 0;
        grub_strncpy(node->filename, filename, sizeof(node->filename));
        grub_snprintf(node->menuentry, sizeof(node->menuentry),
            "menuentry \"%-10s %s\" --class=%s {\n"
            "  vt_set_fake_vlnk \"(%s)%s/%s\" %s %llu\n"
            "  %s_common_menuentry\n"
            "  vt_reset_fake_vlnk\n"
            "}\n",
            grub_get_human_size(fsize, GRUB_HUMAN_SIZE_SHORT), filename, g_menu_class[type],
            g_menu_device, g_menu_path_buf, filename, g_menu_prefix[type], (ulonglong)fsize,
            g_menu_prefix[type]);

    }

    node->prev = NULL;
    node->next = g_browser_list;
    if (g_browser_list)
    {
        g_browser_list->prev = node;
    }
    g_browser_list = node;

    return 0;
}

grub_err_t ventoy_cmd_browser_dir(grub_extcmd_context_t ctxt, int argc, char **args)
{
    int i;
    grub_fs_t fs;
    grub_device_t dev;
    char cfgfile[64];
    browser_node *node;
    browser_mbuf mbuf;

    (void)ctxt;
    (void)argc;

    if (!ventoy_browser_mbuf_alloc(&mbuf))
    {
        return 1;
    }

    fs = (grub_fs_t)grub_strtoul(args[1], NULL, 16);
    if (!fs)
    {
        debug("Invalid fs %s\n", args[1]);
        return 1;
    }
    
    dev = grub_device_open(args[0]);
    if (!dev)
    {
        debug("Failed to open device %s\n", args[0]);
        return 1;
    }
    
    g_menu_fs = fs;
    g_menu_device = args[0];
    g_browser_list = NULL;

    if (args[2][0] == '/' && args[2][1] == 0)
    {
        g_menu_path_len = 0;
        g_menu_path_buf[0] = 0;
        fs->fs_dir(dev, "/", ventoy_browser_iterate_dir, NULL);            
    }
    else
    {
        g_menu_path_len = grub_snprintf(g_menu_path_buf, sizeof(g_menu_path_buf), "%s", args[2]);
        fs->fs_dir(dev, g_menu_path_buf, ventoy_browser_iterate_dir, NULL); 
    }
    grub_device_close(dev);

    browser_ssprintf(&mbuf, "menuentry \"%-10s [../]\" --class=\"vtoyret\" VTOY_RET {\n  "
                     "  echo 'return ...' \n}\n", "<--");

    for (i = 1; i >= 0; i--)
    {
        while (1)
        {
            node = ventoy_browser_find_top_node(i);
            if (node)
            {
                grub_printf("Find Node <%s>\n", node->filename);
                browser_ssprintf(&mbuf, "%s", node->menuentry);
                if (node->prev)
                {
                    node->prev->next = node->next;
                }
                if (node->next)
                {
                    node->next->prev = node->prev;
                }

                if (node == g_browser_list)
                {
                    g_browser_list = node->next;
                }
                grub_free(node);
            }
            else
            {
                break;
            }
        }
    }
    g_browser_list = NULL;
    
    grub_snprintf(cfgfile, sizeof(cfgfile), "configfile mem:0x%lx:size:%d", (ulong)mbuf.buf, mbuf.pos);
    grub_script_execute_sourcecode(cfgfile);

    ventoy_browser_mbuf_free(&mbuf);
    VENTOY_CMD_RETURN(GRUB_ERR_NONE);
}

grub_err_t ventoy_cmd_browser_disk(grub_extcmd_context_t ctxt, int argc, char **args)
{
    char cfgfile[64];
    browser_mbuf mbuf;
    
    (void)ctxt;
    (void)argc;
    (void)args;

    if (!ventoy_browser_mbuf_alloc(&mbuf))
    {
        return 1;
    }

    browser_ssprintf(&mbuf, "menuentry \"%-10s [Return]\" --class=\"vtoyret\" VTOY_RET {\n  "
                     "  echo 'return ...' \n}\n", "<--");

    grub_disk_dev_iterate(ventoy_browser_iterate_disk, &mbuf);

    grub_snprintf(cfgfile, sizeof(cfgfile), "configfile mem:0x%lx:size:%d", (ulong)mbuf.buf, mbuf.pos);
    grub_script_execute_sourcecode(cfgfile);

    ventoy_browser_mbuf_free(&mbuf);
    VENTOY_CMD_RETURN(GRUB_ERR_NONE);
}
