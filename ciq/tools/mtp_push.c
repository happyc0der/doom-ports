/* Push a file into GARMIN/Apps on an MTP-mode Garmin watch (Venu X1 & co).
 * libmtp's own mtp-sendfile leaves storage_id at 0, which this watch
 * rejects ("could not get storage id from parent id"); this sets it.
 *
 *   cc -O2 $(pkg-config --cflags --libs libmtp) -o mtp_push mtp_push.c
 *   ./mtp_push ../bin/DoomCE-venux1.prg DoomCE.prg [GARMIN/Apps]        */
#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static LIBMTP_folder_t *find_child(LIBMTP_folder_t *f, const char *name)
{
    for (; f; f = f->sibling)
        if (f->name && strcmp(f->name, name) == 0) return f;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: mtp_push <local> <remote name> [folder/path]\n"); return 2; }
    const char *local = argv[1], *remote = argv[2], *path = argc > 3 ? argv[3] : "GARMIN/Apps";

    struct stat st;
    if (stat(local, &st) != 0) { perror(local); return 1; }

    LIBMTP_Init();
    LIBMTP_mtpdevice_t *dev = LIBMTP_Get_First_Device();
    if (!dev) { fprintf(stderr, "no MTP device found - is the watch plugged in and unlocked?\n"); return 1; }
    LIBMTP_Get_Storage(dev, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (!dev->storage) { fprintf(stderr, "device has no storage\n"); return 1; }
    uint32_t storage = dev->storage->id;

    /* walk the folder path */
    LIBMTP_folder_t *root = LIBMTP_Get_Folder_List(dev), *cur = NULL;
    char *p = strdup(path), *tok = strtok(p, "/");
    LIBMTP_folder_t *level = root;
    while (tok) {
        cur = find_child(level, tok);
        if (!cur) { fprintf(stderr, "folder '%s' not found in '%s'\n", tok, path); return 1; }
        level = cur->child;
        tok = strtok(NULL, "/");
    }
    if (!cur) { fprintf(stderr, "empty path\n"); return 1; }

    /* replace an existing copy so the watch sees a fresh file */
    LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(dev, storage, cur->folder_id);
    for (LIBMTP_file_t *f = files; f; f = f->next)
        if (f->filename && strcmp(f->filename, remote) == 0) {
            printf("removing old %s (id %u)\n", remote, f->item_id);
            LIBMTP_Delete_Object(dev, f->item_id);
        }

    LIBMTP_file_t *g = LIBMTP_new_file_t();
    g->filesize   = st.st_size;
    g->filename   = strdup(remote);
    g->filetype   = LIBMTP_FILETYPE_UNKNOWN;
    g->parent_id  = cur->folder_id;
    g->storage_id = storage;

    printf("sending %s -> %s/%s (%lld bytes, folder %u, storage %u)\n",
           local, path, remote, (long long)st.st_size, cur->folder_id, storage);
    int rc = LIBMTP_Send_File_From_File(dev, local, g, NULL, NULL);
    if (rc != 0) { LIBMTP_Dump_Errorstack(dev); LIBMTP_Clear_Errorstack(dev); fprintf(stderr, "send failed\n"); }
    else printf("done: item id %u\n", g->item_id);
    LIBMTP_destroy_file_t(g);
    LIBMTP_Release_Device(dev);
    return rc;
}
