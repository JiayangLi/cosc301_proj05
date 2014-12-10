#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

#define TOTAL_CLUSTERS(bpb) (bpb->bpbSectors / bpb->bpbSecPerClust)

int follow_file(uint16_t cluster, uint8_t *img_buf, struct bpb33 *bpb){

}

/* parse a given dirent, returns the starting cluster if the given
dirent indicates a directory and 0 otherwise */
uint16_t parse_dirent(struct direntry *dirent, uint8_t *img_buf, struct bpb33 *bpb, char *ref_clusters){
    uint16_t subdir_cluster = 0;  //initialize to an invalid cluster

    char name[9];
    char extension[4];
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

    int i;

    if (((uint8_t)name[0]) == SLOT_EMPTY){ //no more stuff in this dir
        return subdir_cluster;
    }

    if (((uint8_t)name[0]) == SLOT_DELETED){ //skip deleted entry  
        return subdir_cluster;
    }

    if (((uint8_t)name[0]) == 0x2E){    //skip "." or ".."
        return subdir_cluster;
    }

    for (i = 8; i > 0; i--){    //remove padded spaces in name
        if (name[i] == ' ') 
            name[i] = '\0';
        else 
            break;
    }

    for (i = 3; i > 0; i--){    //remove padded spaces in extension
    if (extension[i] == ' ') 
        extension[i] = '\0';
    else 
        break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
    // ignore any long file name extension entries
    //
    // printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){    
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
            //a (non-hidden) dir entry
            subdir_cluster = getushort(dirent->deStartCluster);
        }
    } else {
        //a "regular" file entry
        uint32_t size_from_dirent = getulong(dirent->deFileSize);
        int size_from_chain = follow_file(dirent->deStartCluster, img_buf, bpb);
        /* !!! do size fix here !!! */
    }

    return subdir_cluster;
}

void follow_dir(uint16_t cluster_num, uint8_t *img_buf, struct bpb33 *bpb, char *ref_clusters){

}

char *traverse_root(uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t cluster = 0;   //indicates root directory

    struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

    //ref_clusters keeps track of clusters referenced by some dirent metadata
    char *ref_clusters = (char *) malloc(sizeof(char) * TOTAL_CLUSTERS(bpb));
    memset(ref_clusters, 0, sizeof(char) * TOTAL_CLUSTERS(bpb));

    int i = 0;
    for (; i < bpb->bpbRootDirEnts ;i++){   //go to every entry in root dir
        uint16_t subdir_cluster = parse_dirent(dirent, ref_clusters);
        if (is_valid_cluster(subdir_cluster, bpb)){
            follow_dir(subdir_cluster, img_buf, bpb, ref_clusters);
        }
        dirent++;   //still in root dir, just increment to get next dir entry
    }
    return ref_clusters;
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


int main(int argc, char** argv) {
    uint8_t *img_buf;
    int fd;
    struct bpb33* bpb;

    char *ref_clusters;

    if (argc < 2) {
	   usage(argv[0]);
    }

    img_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(img_buf);
    
    // your code should start here...
    ref_clusters = traverse_root(img_buf, bpb);

    unmmap_file(img_buf, &fd);
    free(ref_clusters);
    return 0;
}
