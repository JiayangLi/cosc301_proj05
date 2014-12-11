#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include <assert.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

#define TOTAL_CLUSTERS(bpb) (bpb->bpbSectors / bpb->bpbSecPerClust)
#define CLUSTER_SIZE(bpb) (bpb->bpbSecPerClust * bpb->bpbBytesPerSec)

void follow_file(uint16_t cluster, uint32_t size, uint8_t *img_buf, struct bpb33 *bpb, char *ref_clusters){
    uint32_t size_from_dirent = size;
    uint16_t last_fat_entry = 0;
    uint32_t chain_size = 0;
    //printf("before size: %d\n", size);
    
    //assert(cluster != 0);
    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!! */

        chain_size += CLUSTER_SIZE(bpb);

        if (size < CLUSTER_SIZE(bpb)){ //should be the last cluster according to dirent size
            last_fat_entry = cluster;
            cluster = get_fat_entry(cluster, img_buf, bpb);
            break;
        }
        size -= CLUSTER_SIZE(bpb);

        cluster = get_fat_entry(cluster, img_buf, bpb);
    }

    // printf("cluster number: %d\n", cluster);
    //printf("chain size: %d\n", chain_size);
    if (is_valid_cluster(cluster, bpb)){    //still in the middle of a chain, free following clusters
        /* !!! free clusters beginning from cluster !!! */
        printf("chain size (%d) greater than dirent size (%d)\n", chain_size, size_from_dirent);
    } else if (size_from_dirent > chain_size){  //reached the end of chain, but dirent size is still too big
        /* !!! adjust dirent size !!! */
        printf("chain size (%d) less than dirent size (%d)\n", chain_size, size_from_dirent);
    } else {
        printf("normal file!\n");
    }
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

    // if ((dirent->deAttributes & ATTR_NORMAL) == ATTR_NORMAL){   //"regular" file
    //     uint32_t size_from_dirent = getulong(dirent->deFileSize);
    //     printf("starting cluster %d\n", getushort(dirent->deStartCluster));
    //     follow_file(getushort(dirent->deStartCluster), size_from_dirent, img_buf, bpb, ref_clusters);
    // } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){    
    //     if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
    //         //a (non-hidden) dir entry
    //         subdir_cluster = getushort(dirent->deStartCluster);
    //     }
    // } 

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
        /* skip long file name */
    } else if ((dirent->deAttributes & ATTR_VOLUME) != 0){
        /* skip volume */
    } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){
        /* skip hidden dir */
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
            // a normal dir
            subdir_cluster = getushort(dirent->deStartCluster);
        }
    } else {
        // a normal file
        uint32_t size_from_dirent = getulong(dirent->deFileSize);
        uint16_t starting_cluster = getushort(dirent->deStartCluster);
        follow_file(starting_cluster, size_from_dirent, img_buf, bpb, ref_clusters);
    }

    return subdir_cluster;
}

void follow_dir(uint16_t cluster, uint8_t *img_buf, struct bpb33 *bpb, char *ref_clusters){
    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!! */
        struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        for (int i = 0; i < numDirEntries; i++){    //parse every direntry and follow subdir if any
            uint16_t subdir_cluster = parse_dirent(dirent, img_buf, bpb, ref_clusters);
            if (subdir_cluster){
                follow_dir(subdir_cluster, img_buf, bpb, ref_clusters);
            }
            dirent++;
        }
        cluster = get_fat_entry(cluster, img_buf, bpb);
    }
}

char *traverse_root(uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t cluster = 0;   //indicates root directory

    struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

    //ref_clusters keeps track of clusters referenced by some dirent metadata
    char *ref_clusters = (char *) malloc(sizeof(char) * TOTAL_CLUSTERS(bpb));
    memset(ref_clusters, 0, sizeof(char) * TOTAL_CLUSTERS(bpb));

    for (int i = 0; i < bpb->bpbRootDirEnts ;i++){   //go through every entry in root dir
        uint16_t subdir_cluster = parse_dirent(dirent, img_buf, bpb, ref_clusters);
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
