#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "protocol.h"
#include "render_config.h"
#include "dir_utils.h"

// Build parent directories for the specified file name
// Note: the part following the trailing / is ignored
// e.g. mkdirp("/a/b/foo.png") == shell mkdir -p /a/b
int mkdirp(const char *path) {
    struct stat s;
    char tmp[PATH_MAX];
    char *p;

    strncpy(tmp, path, sizeof(tmp));

    // Look for parent directory
    p = strrchr(tmp, '/');
    if (!p)
        return 0;

    *p = '\0';

    if (!stat(tmp, &s))
        return !S_ISDIR(s.st_mode);
    *p = '/';
    // Walk up the path making sure each element is a directory
    p = tmp;
    if (!*p)
        return 0;
    p++; // Ignore leading /
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (!stat(tmp, &s)) {
                if (!S_ISDIR(s.st_mode)) {
                    fprintf(stderr, "Error, is not a directory: %s\n", tmp);
                    return 1;
                }
            } else if (mkdir(tmp, 0777)) {
                    // Ignore multiple threads attempting to create the same directory
                    if (errno != EEXIST) { 
                       perror(tmp);
                       return 1;
                    }
                }
            *p = '/';
        }
        p++;
    }
    return 0;
}



/* File path hashing. Used by both mod_tile and render daemon
 * The two must both agree on the file layout for meta-tiling
 * to work
 */

void xyz_to_path(char *path, size_t len, const char *tile_dir, const char *xmlconfig, int x, int y, int z)
{
#ifdef DIRECTORY_HASH
    // We attempt to cluster the tiles so that a 16x16 square of tiles will be in a single directory
    // Hash stores our 40 bit result of mixing the 20 bits of the x & y co-ordinates
    // 4 bits of x & y are used per byte of output
    unsigned char i, hash[5];

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
    snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.png", tile_dir, xmlconfig, z, hash[4], hash[3], hash[2], hash[1], hash[0]);
#else
    snprintf(path, len, TILE_PATH "/%s/%d/%d/%d.png", xmlconfig, z, x, y);
#endif
    return;
}

int check_xyz(int x, int y, int z)
{
    int oob, limit;

    // Validate tile co-ordinates
    oob = (z < 0 || z > MAX_ZOOM);
    if (!oob) {
         // valid x/y for tiles are 0 ... 2^zoom-1
        limit = (1 << z) - 1;
        oob =  (x < 0 || x > limit || y < 0 || y > limit);
    }

    if (oob)
        fprintf(stderr, "got bad co-ords: x(%d) y(%d) z(%d)\n", x, y, z);

    return oob;
}


int path_to_xyz(const char *path, char *xmlconfig, int *px, int *py, int *pz)
{
#ifdef DIRECTORY_HASH
    int i, n, hash[5], x, y, z;

    n = sscanf(path, HASH_PATH "/%40[^/]/%d/%d/%d/%d/%d/%d", xmlconfig, pz, &hash[0], &hash[1], &hash[2], &hash[3], &hash[4]);
    if (n != 7) {
        fprintf(stderr, "Failed to parse tile path: %s\n", path);
        return 1;
    } else {
        x = y = 0;
        for (i=0; i<5; i++) {
            if (hash[i] < 0 || hash[i] > 255) {
                fprintf(stderr, "Failed to parse tile path (invalid %d): %s\n", hash[i], path);
                return 2;
            }
            x <<= 4;
            y <<= 4;
            x |= (hash[i] & 0xf0) >> 4;
            y |= (hash[i] & 0x0f);
        }
        z = *pz;
        *px = x;
        *py = y;
        return check_xyz(x, y, z);
    }
#else
    int n;
    n = sscanf(path, TILE_PATH "/%40[^/]/%d/%d/%d", xmlconfig, pz, px, py);
    if (n != 4) {
        fprintf(stderr, "Failed to parse tile path: %s\n", path);
        return 1;
    } else {
        return check_xyz(*px, *py, *pz);
    }
#endif
}

#ifdef METATILE
// Returns the path to the meta-tile and the offset within the meta-tile
int xyz_to_meta(char *path, size_t len, const char *tile_dir, const char *xmlconfig, int x, int y, int z)
{
    unsigned char i, hash[5], offset, mask;

    // Each meta tile winds up in its own file, with several in each leaf directory
    // the .meta tile name is beasd on the sub-tile at (0,0)
    mask = METATILE - 1;
    offset = (x & mask) * METATILE + (y & mask);
    x &= ~mask;
    y &= ~mask;

    for (i=0; i<5; i++) {
        hash[i] = ((x & 0x0f) << 4) | (y & 0x0f);
        x >>= 4;
        y >>= 4;
    }
#ifdef DIRECTORY_HASH
    snprintf(path, len, "%s/%s/%d/%u/%u/%u/%u/%u.meta", tile_dir, xmlconfig, z, hash[4], hash[3], hash[2], hash[1], hash[0]);
#else
    snprintf(path, len, "%s/%s/%d/%u/%u.meta", tile_dir, xmlconfig, z, x, y);
#endif
    return offset;
}
#endif

time_t getPlanetTimestamp(char *tile_dir, char *mapname)
{
    static time_t last_planet_check;
    static time_t last_map_check[XMLCONFIGS_MAX];
    static time_t planet_timestamp;
    static time_t map_timestamp[XMLCONFIGS_MAX];
    time_t now = time(NULL);
    struct stat buf;
    char filename[PATH_MAX];
    int mapnumber;

    if(mapname != NULL){
        mapnumber = getMapNumber(mapname);

        // Only check for updates periodically
        if (now < last_map_check[mapnumber] + 300){
            return map_timestamp[mapnumber];
        }

        last_map_check[mapnumber] = now;
        snprintf(filename, PATH_MAX-1, "%s/%s/%s", tile_dir, mapname, PLANET_TIMESTAMP);

        if (!stat(filename, &buf)) {
            return map_timestamp[mapnumber] = buf.st_mtime;
        }
    }

    // Only check for updates periodically
    if (now < last_planet_check + 300){
        return planet_timestamp;
    }
    last_planet_check = now;

    // specific planet time missing, check for gloabal
    snprintf(filename, PATH_MAX-1, "%s/%s", tile_dir, PLANET_TIMESTAMP);
    if (stat(filename, &buf)) {
        fprintf(stderr, "Planet timestamp file (%s) is missing\n", filename);
        // Make something up
        planet_timestamp = now - 3 * 24 * 60 * 60;
    }else{
        planet_timestamp = buf.st_mtime;
    }
    return planet_timestamp;
}

int getMapNumber(char *mapname)
{
    static char *mapnames[XMLCONFIGS_MAX];
    int i;

    if(mapname == NULL){
        return 0;
    }

    for (i = 0; i < XMLCONFIGS_MAX; i++){
        if(mapnames[i] != NULL){
            if(strcmp(mapnames[i], mapname) == 0){
                return i;
            }
        }else{
            mapnames[i] = strdup(mapname);
            return i;
        }
    }
}
