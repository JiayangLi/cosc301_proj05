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

/* helper functions related to managing orphans */
typedef struct {
    uint16_t *cluster_p;
    int list_size;  //actual size of the array
    int orphan_size; //how many cluster currently in this orphan
} orphan;

void orphan_init(orphan *orp){
    orp->list_size = 5; //default size 5
    orp->orphan_size = 0;
    orp->cluster_p = (uint16_t *) malloc(sizeof(uint16_t) * orp->list_size);
}

void orphan_add(orphan *orp, uint16_t first, uint16_t second){
    if (orp->orphan_size == orp->list_size){ //need to resize
        orp->list_size = orp->list_size * 2;
        orp->cluster_p = (uint16_t *) realloc(orp->cluster_p, sizeof(uint16_t) * orp->list_size);
    }

    //add here
    if (orp->orphan_size == 0){ // orphan list is empty initially
        orp->cluster_p[0] = first;
        orp->cluster_p[1] = second;
        orp->orphan_size = 2;
    } else if (first == orp->cluster_p[orp->orphan_size - 1]){ //new orphan tail
        orp->cluster_p[orp->orphan_size] = second;
        orp->orphan_size++;
    } else if (second == orp->cluster_p[0]){   //new orphan head
        uint16_t *chain = orp->cluster_p;
        orp->cluster_p = (uint16_t *) malloc(sizeof(uint16_t) * orp->list_size);
        orp->cluster_p[0] = first;
        memcpy(&(orp->cluster_p[1]), chain, sizeof(uint16_t) * (orp->orphan_size));
        orp->orphan_size++;
        free(chain); 
    } else { //should not happen
        fprintf(stderr, "orphan_add is wrong!\n");
    }
}

void orphan_print(orphan *orp){
    printf("printing one orphan:\n");
    for (int i = 0; i < orp->orphan_size; i++){
        printf("%d ", orp->cluster_p[i]);
    }
    printf("\nend of one orphan\n");
}

void orphan_destroy(orphan *orp){
    free(orp->cluster_p);
    free(orp);
}

/* -------------------------------------------- */

/* Mark the given cluster as referenced
 * 0 - unreferenced
 * 1 - referenced
 * ref should have been initialized to all 0's */
void update_ref(uint16_t cluster, char *ref){
    ref[cluster] = 1;
}

void follow_orphan(uint16_t cluster, uint8_t *img_buf, struct bpb33 *bpb, char *ref){
    orphan *orp = (orphan *) malloc(sizeof(orphan));
    orphan_init(orp);

    uint16_t next_cluster;

    while (is_valid_cluster(cluster, bpb)){
        //need to mark this entire orphan chain as "referenced"
        //to ensure each orphan is visited once
        update_ref(cluster, ref);  

        next_cluster = get_fat_entry(cluster, img_buf, bpb);
        orphan_add(orp, cluster, next_cluster);
        cluster = next_cluster;
    }
    orphan_print(orp);
    orphan_destroy(orp);
}


int is_chained(uint16_t cluster, struct bpb33 *bpb){
    // if (!is_valid_cluster(cluster, bpb)){
    //     //printf("%d\n", cluster);
    //     return 0;}
    if (cluster >= (CLUST_RSRVDS & FAT12_MASK) && cluster <= (CLUST_RSRVDE & FAT12_MASK))
        return 0;
    else if (cluster == (CLUST_BAD & FAT12_MASK))
        return 0;
    // else if (cluster < (CLUST_FIRST & FAT12_MASK))
    //     return false;
    else if (cluster == (CLUST_FREE & FAT12_MASK))
        return 0;
    else
        return 1;
}

void traverse_ref(char *ref, uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t fat_value;
    for(int i = 2; i < TOTAL_CLUSTERS(bpb); i++){
        if (ref[i] == 0){
            fat_value = get_fat_entry(i, img_buf, bpb);
            //printf("fat is %d \n", fat_value);
            if (is_chained(fat_value, bpb)){
                follow_orphan(i, img_buf, bpb, ref);
            }
        }
    }
}

/* Free all clusters starting from the given cluster*/
void free_clusters(uint16_t cluster, uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t next_cluster;

    while (is_valid_cluster(cluster, bpb)){
        next_cluster = get_fat_entry(cluster, img_buf, bpb);
        set_fat_entry(cluster, FAT12_MASK&CLUST_FREE, img_buf, bpb);
        cluster = next_cluster;
    }
}

/* Returns the chain size if needed to update dirent size, 0 otherwise */
uint32_t follow_file(uint16_t cluster, uint32_t size, uint8_t *img_buf, struct bpb33 *bpb, char *ref, char *path){
    uint32_t size_from_dirent = size;
    uint16_t last_fat_entry = 0;
    uint32_t chain_size = 0;
    //printf("before size: %d\n", size);
    
    //assert(cluster != 0);
    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!! */
        update_ref(cluster, ref);

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
        printf("%s: chain size (>%d) greater than dirent size (%d)\n", path, chain_size, size_from_dirent);
         
        /* !!! fix chain > dirent size issue - truncate and free clusters !!! */
        printf("Truncating the file and releasing extra clusters...\n");
        set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
        free_clusters(cluster, img_buf, bpb);

    } else if (size_from_dirent > chain_size){  //reached the end of chain, but dirent size is still too big
        printf("%s: chain size (%d) less than dirent size (%d)\n", path, chain_size, size_from_dirent);
        return chain_size;
    } else {
        printf("%s: normal file!\n", path);
    }

    return 0;
}

/* parse a given dirent, returns the starting cluster if the given
dirent indicates a directory and 0 otherwise */
uint16_t parse_dirent(struct direntry *dirent, uint8_t *img_buf, struct bpb33 *bpb, char *ref, char *path){
    uint16_t subdir_cluster = 0;  //initialize to an invalid cluster

    char name[9];
    char extension[4];
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);

    //char *fullname = (char *) malloc(MAXPATHLEN * sizeof(char)); //holds the name of this file or dir
    //strcpy(fullname, buffer); //buffer holds the parent path

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

    strcat(path, name); //append name first, append extension later if needed

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
        /* skip long file name */
    } else if ((dirent->deAttributes & ATTR_VOLUME) != 0){
        /* skip volume */
    } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){
        /* skip hidden dir */
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
            // a normal dir
            strcat(path, "/");
            subdir_cluster = getushort(dirent->deStartCluster);
        }
    } else {
        // a normal file
        strcat(path, ".");
        strcat(path, extension); //append the extension since it's a file

        uint32_t size_from_dirent = getulong(dirent->deFileSize);
        uint16_t starting_cluster = getushort(dirent->deStartCluster);
        uint32_t chain_size = follow_file(starting_cluster, size_from_dirent, img_buf, bpb, ref, path);

        if (chain_size){
            /* !!! fix dirent size > chain issue - adjust dirent size !!! */
            printf("Changing directory entry size metadata to %d...\n", chain_size);
            putulong(dirent->deFileSize, chain_size);
        }
    }

    return subdir_cluster;
}

void follow_dir(uint16_t cluster, uint8_t *img_buf, struct bpb33 *bpb, char *ref, char *path){
    char pathcopy[MAXPATHLEN];
    

    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!! */
        update_ref(cluster, ref);

        struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        for (int i = 0; i < numDirEntries; i++){    //parse every direntry and follow subdir if any
            strcpy(pathcopy, path); //intilizes pathcopy to this dir's path for every entry

            uint16_t subdir_cluster = parse_dirent(dirent, img_buf, bpb, ref, pathcopy);
            if (subdir_cluster){
                follow_dir(subdir_cluster, img_buf, bpb, ref, pathcopy);
            }
            dirent++;
        }
        cluster = get_fat_entry(cluster, img_buf, bpb);
    }
}

char *traverse_root(uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t cluster = 0;   //indicates root directory
    struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

    char path[MAXPATHLEN];

    //ref keeps track of clusters referenced by some dirent metadata
    char *ref = (char *) malloc(sizeof(char) * TOTAL_CLUSTERS(bpb));
    memset(ref, 0, TOTAL_CLUSTERS(bpb));

    for (int i = 0; i < bpb->bpbRootDirEnts ;i++){   //go through every entry in root dir
        strcpy(path, "/"); //reinitialize path back to "/" for the next root dir entry

        uint16_t subdir_cluster = parse_dirent(dirent, img_buf, bpb, ref, path);
        if (is_valid_cluster(subdir_cluster, bpb)){
            follow_dir(subdir_cluster, img_buf, bpb, ref, path);
        }
        dirent++;   //still in root dir, just increment to get next dir entry
    }
    return ref;
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

/* debugging helpers */
void print_ref(char *ref){
    for (int i = 0; i < 2880; i++){
        // if (ref[i] == 0)
        //     printf("%c ", 'x');
        // else if (i == 2879)
        //     printf("2879\n");
        // else
        //     printf("%c ", 'p');
        if (i == 2879)
            printf("2879\n");
    }
}

/* --------------------- */

int main(int argc, char** argv) {
    uint8_t *img_buf;
    int fd;
    struct bpb33* bpb;

    char *ref; //keeps track of clusters referenced by some dir entry metadata


    img_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(img_buf);
    
    // your code should start here...
    ref = traverse_root(img_buf, bpb);

    printf("checking for orphans...\n");
    traverse_ref(ref, img_buf, bpb);
    printf("Finished checking for orphans...\n");

    // set_fat_entry(500, 600, img_buf, bpb);
    // set_fat_entry(600, FAT12_MASK&CLUST_EOFS, img_buf, bpb);

    // printf("cluster 500: %d\n", get_fat_entry(500, img_buf, bpb));
    // printf("cluster 600: %d\n", get_fat_entry(600, img_buf, bpb));

    unmmap_file(img_buf, &fd);
    free(bpb);
    free(ref);
    return 0;
}
