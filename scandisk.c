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

typedef struct orphans_node{
    orphan *one_orphan;
    struct orphans_node *next;
} orphans_node;

void orphan_init(orphan *orp){
    orp->list_size = 5; //default size 5
    orp->orphan_size = 0;
    orp->cluster_p = (uint16_t *) malloc(sizeof(uint16_t) * orp->list_size);
}

int orphan_add(orphan *orp, uint16_t first, uint16_t second){
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
    } else {
        return 0;
    }
    return 1;
}

void fix_orphan_EOF(orphan *orp){
    uint16_t last = orp->cluster_p[orp->orphan_size - 1];
    if (!is_end_of_file(last)){
        orphan_add(orp, last, FAT12_MASK & CLUST_EOFS);
    }
}

void orphan_print(orphan *orp){
    printf("printing an orphan chain: ");
    for (int i = 0; i < orp->orphan_size; i++){
        printf("%d ", orp->cluster_p[i]);
    }
    printf("\n");
}

void orphan_destroy(orphan *orp){
    free(orp->cluster_p);
    free(orp);
}

void orphans_list_add(orphans_node **list, uint16_t first, uint16_t second){
    if (*list == NULL){ //first orphan encountered
        orphan *add = (orphan *) malloc(sizeof(orphan));
        orphan_init(add);
        orphan_add(add, first, second);

        orphans_node *new_orphan = (orphans_node *) malloc(sizeof(orphans_node));
        new_orphan->one_orphan = add;
        new_orphan->next = NULL;

        *list = new_orphan;
    } else { //need to iterate through the orphans list
        orphans_node *temp = *list;
        orphans_node *tail = temp;

        for (; temp != NULL; temp = temp->next){
            if (orphan_add(temp->one_orphan, first, second)){ //part of existing orphan
                return;
            }
            if (temp->next == NULL){ //update tail
                tail = temp;
            }
        }

        //new orphan
        orphan *add = (orphan *) malloc(sizeof(orphan));
        orphan_init(add);
        orphan_add(add, first, second);

        orphans_node *new_orphan = (orphans_node *) malloc(sizeof(orphans_node));
        new_orphan->one_orphan = add;
        new_orphan->next = NULL;

        tail->next = new_orphan;
    }
}

void orphans_list_clear(orphans_node *list){
    orphans_node *temp = list;
    while (list != NULL){
        orphan_destroy(list->one_orphan);
        list = list->next;
        free(temp);
        temp = list;
    }
}
/* --------end of orphan management helpers-------------- */

void write_dirent(struct direntry *dirent, uint16_t starting_cluster, uint32_t size, int orphan_id){
    printf("Getting orphan%d (starting from: %d, size: %d) home as FOUND%d.DAT\n", orphan_id, starting_cluster, size, orphan_id);

    char id[4];

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    // name and extension
    sprintf(id, "%d", orphan_id);
    memset(dirent->deName, ' ', 8);
    memcpy(&(dirent->deName[0]), "FOUND", 5);
    memcpy(&(dirent->deName[5]), id, strlen(id));

    memcpy(dirent->deExtension, "dat", 3);

    // attribute, starting_cluster and size
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, starting_cluster);
    putulong(dirent->deFileSize, size);
}

void get_orphan_home(orphan *orp, uint8_t *img_buf, struct bpb33 *bpb, int orphan_id){
    uint16_t cluster = 0;   //indicates root directory
    struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

    for (int i = 0; i < bpb->bpbRootDirEnts ;i++){   //go through every entry in root dir
        if (dirent->deName[0] == SLOT_EMPTY){   //empty dirent found
            uint16_t starting_cluster = orp->cluster_p[0];
            uint32_t size = (orp->orphan_size - 1) * CLUSTER_SIZE(bpb); // minus 1 for EOF
            write_dirent(dirent, starting_cluster, size, orphan_id);
            dirent++;

            /* make sure the next dirent is set to be empty, just in
               case it wasn't before */
            memset((uint8_t*)dirent, 0, sizeof(struct direntry));
            dirent->deName[0] = SLOT_EMPTY;
            return;
        }
        dirent++;   //still in root dir, just increment to get next dir entry
    }
    fprintf(stderr, "No more available entry in root directory! Give up!\n");
    exit(EXIT_FAILURE);
}

/* Mark the given cluster as referenced
 * 0 - unreferenced
 * 1 - referenced
 * ref should have been initialized to all 0's */
int update_ref(uint16_t cluster, char *ref){
    if (ref[cluster]){
        return 1;
    }
    ref[cluster] = 1;
    return 0;
}

int is_chained(uint16_t cluster){
    if (cluster >= (CLUST_RSRVDS & FAT12_MASK) && cluster <= (CLUST_RSRVDE & FAT12_MASK))
        return 0;
    else if (cluster == (CLUST_BAD & FAT12_MASK))
        return 0;
    else if (cluster == (CLUST_FREE & FAT12_MASK))
        return 0;
    else
        return 1;
}

void traverse_ref(char *ref, uint8_t *img_buf, struct bpb33 *bpb){
    uint16_t fat_value;
    int orphan_id = 1;
    orphans_node *orphans_list = NULL;

    for(int i = 2; i < TOTAL_CLUSTERS(bpb); i++){
        if (ref[i] == 0){
            fat_value = get_fat_entry(i, img_buf, bpb);
            if (is_chained(fat_value)){
                if (fat_value == (CLUST_BAD & FAT12_MASK) || fat_value == (CLUST_FREE & FAT12_MASK) || ref[fat_value]){
                    orphans_list_add(&orphans_list, i, (CLUST_EOFS & FAT12_MASK));
                } else {
                    orphans_list_add(&orphans_list, i, fat_value);
                }             
            }
        }
    }
    orphans_node *to_free = orphans_list;

    for (; orphans_list != NULL; orphans_list = orphans_list->next){
        fix_orphan_EOF(orphans_list->one_orphan);
        orphan_print(orphans_list->one_orphan);
        get_orphan_home(orphans_list->one_orphan, img_buf, bpb, orphan_id);
        orphan_id++;
    }
    
    orphans_list_clear(to_free);
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
    int has_bad_sector = 0;
    int has_free_sector = 0;
    //printf("before size: %d\n", size);
    
    //assert(cluster != 0);
    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!!
            if overlap, change EOF */
        if (update_ref(cluster, ref)){
            printf("Chain overlap found, truncating FAT chain...\n");
            set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
            cluster = (CLUST_FREE & FAT12_MASK);
            break;
        }

        chain_size += CLUSTER_SIZE(bpb);

        if (size < CLUSTER_SIZE(bpb)){ //should be the last cluster according to dirent size
            last_fat_entry = cluster;
            cluster = get_fat_entry(cluster, img_buf, bpb);
            has_bad_sector = (cluster == (CLUST_BAD & FAT12_MASK));
            has_free_sector = (cluster == (CLUST_FREE & FAT12_MASK));
            break;
        }
        size -= CLUSTER_SIZE(bpb);

        last_fat_entry = cluster;
        cluster = get_fat_entry(cluster, img_buf, bpb);
        has_bad_sector = (cluster == (CLUST_BAD & FAT12_MASK));
        has_free_sector = (cluster == (CLUST_FREE & FAT12_MASK));
    }

    /* Fix any possible in-chain bad cluster */
    if (has_bad_sector){ 
        printf("Bad sector found in %s, truncating FAT chain...\n", path);
        set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
    }

    /* Fix any possible in-chain free cluster */
    if (has_free_sector){
        printf("Free sector found in %s, truncating FAT chain...\n", path);
        set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
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

            //delete entry if the starting cluster is bad
            if (subdir_cluster == (CLUST_BAD & FAT12_MASK) || ref[subdir_cluster] || subdir_cluster == (CLUST_FREE & FAT12_MASK)){
                printf("Deleting %s because of bad starting cluster(or duplicate references or free cluster)...\n", path);
                dirent->deName[0] = SLOT_DELETED;
                return 0;
            }
        }
    } else {
        // a normal file
        strcat(path, ".");
        strcat(path, extension); //append the extension since it's a file

        uint32_t size_from_dirent = getulong(dirent->deFileSize);
        uint16_t starting_cluster = getushort(dirent->deStartCluster);

        //delete entry if the starting cluster is bad
        if (starting_cluster == (CLUST_BAD & FAT12_MASK) || ref[starting_cluster] || starting_cluster == (CLUST_FREE & FAT12_MASK)){
            printf("Deleting %s entry because of bad starting cluster(or duplicate references or free cluster)...\n", path);
            dirent->deName[0] = SLOT_DELETED;
            return 0;
        }

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
    uint16_t last_fat_entry;
    int has_bad_sector = 0;
    int has_free_sector = 0;

    char pathcopy[MAXPATHLEN];
    

    while (is_valid_cluster(cluster, bpb)){
        /* !!! mark this cluster referenced here !!! */
        update_ref(cluster, ref);

        struct direntry *dirent = (struct direntry *) cluster_to_addr(cluster, img_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        for (int i = 0; i < numDirEntries; i++){    //parse every direntry and follow subdir if any
            strcpy(pathcopy, path); //intilizes pathcopy to this dir's path for every entry

            uint16_t subdir_cluster = parse_dirent(dirent, img_buf, bpb, ref, pathcopy);
            if (is_valid_cluster(subdir_cluster, bpb)){
                follow_dir(subdir_cluster, img_buf, bpb, ref, pathcopy);
            }
            dirent++;
        }
        last_fat_entry = cluster;
        cluster = get_fat_entry(cluster, img_buf, bpb);
        has_bad_sector = (cluster == (CLUST_BAD & FAT12_MASK));
        has_free_sector = (cluster == (CLUST_FREE & FAT12_MASK));
    }

    /* Fix any possible in-chain bad cluster */
    if (has_bad_sector){ 
        printf("Bad sector found in %s, truncating FAT chain...\n", path);
        set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
    }

    /* Fix any possible in-chain free cluster */
    if (has_free_sector){ 
        printf("Free sector found in %s, truncating FAT chain...\n", path);
        set_fat_entry(last_fat_entry, FAT12_MASK&CLUST_EOFS, img_buf, bpb);
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

int main(int argc, char** argv) {
    uint8_t *img_buf;
    int fd;
    struct bpb33* bpb;

    char *ref; //keeps track of clusters referenced by some dir entry metadata


    img_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(img_buf);
    
    // your code should start here...
    ref = traverse_root(img_buf, bpb);

    printf("\nStart checking for orphans...\n");
    traverse_ref(ref, img_buf, bpb);
    printf("Finished checking for orphans...\n");

    unmmap_file(img_buf, &fd);
    free(bpb);
    free(ref);
    return 0;
}
