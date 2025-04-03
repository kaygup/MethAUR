#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <json-c/json.h>

#define MAX_PACKAGES 50
#define MAX_BUFFER 8192
#define AUR_RPC_URL "https://aur.archlinux.org/rpc/?v=5&type=search&arg="
#define AUR_PKG_URL "https://aur.archlinux.org/cgit/aur.git/snapshot/"
#define TMP_DIR "/tmp/methaur/"

// Structure to hold package information
typedef struct {
    char *name;
    char *version;
    char *description;
    int votes;
    char *maintainer;
    char *url;
} Package;

// Structure for curl data
typedef struct {
    char *data;
    size_t size;
} CurlData;

// Function declarations
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void search_packages(const char *query, Package **results, int *count);
void display_search_results(Package *results, int count);
int install_package(const char *package_name);
int remove_package(const char *package_name);
void free_package_data(Package *packages, int count);
int download_and_build_package(const char *package_name);
void create_directories();
void print_usage();

// Callback function for curl
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    CurlData *mem = (CurlData *)userp;
    
    char *ptr = realloc(mem->data, mem->size + real_size + 1);
    if(!ptr) {
        fprintf(stderr, "Error: Out of memory\n");
        return 0;
    }
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = 0;
    
    return real_size;
}

// Search for packages
void search_packages(const char *query, Package **results, int *count) {
    CURL *curl;
    CURLHcode res;
    CurlData chunk;
    char url[MAX_BUFFER];
    
    chunk.data = malloc(1);
    chunk.size = 0;
    
    curl = curl_easy_init();
    if(curl) {
        snprintf(url, MAX_BUFFER, "%s%s", AUR_RPC_URL, query);
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "methaur/1.0");
        
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK) {
            fprintf(stderr, "Error: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            *count = 0;
            return;
        }
        
        struct json_object *root, *results_obj, *package_obj;
        struct json_object *name_obj, *version_obj, *desc_obj, *votes_obj, *maintainer_obj, *url_obj;
        
        root = json_tokener_parse(chunk.data);
        json_object_object_get_ex(root, "results", &results_obj);
        
        int num_results = json_object_array_length(results_obj);
        *count = (num_results > MAX_PACKAGES) ? MAX_PACKAGES : num_results;
        
        *results = (Package *)malloc(sizeof(Package) * (*count));
        
        for(int i = 0; i < *count; i++) {
            package_obj = json_object_array_get_idx(results_obj, i);
            
            json_object_object_get_ex(package_obj, "Name", &name_obj);
            json_object_object_get_ex(package_obj, "Version", &version_obj);
            json_object_object_get_ex(package_obj, "Description", &desc_obj);
            json_object_object_get_ex(package_obj, "NumVotes", &votes_obj);
            json_object_object_get_ex(package_obj, "Maintainer", &maintainer_obj);
            json_object_object_get_ex(package_obj, "URL", &url_obj);
            
            (*results)[i].name = strdup(json_object_get_string(name_obj));
            (*results)[i].version = strdup(json_object_get_string(version_obj));
            (*results)[i].description = strdup(json_object_get_string(desc_obj));
            (*results)[i].votes = json_object_get_int(votes_obj);
            (*results)[i].maintainer = strdup(json_object_get_string(maintainer_obj));
            (*results)[i].url = strdup(json_object_get_string(url_obj));
        }
        
        json_object_put(root);
        curl_easy_cleanup(curl);
    }
    
    free(chunk.data);
}

// Display search results
void display_search_results(Package *results, int count) {
    printf("\n");
    printf("%-3s %-25s %-15s %-8s %-15s %s\n", "ID", "Name", "Version", "Votes", "Maintainer", "Description");
    printf("-----------------------------------------------------------------------------------------\n");
    
    for(int i = 0; i < count; i++) {
        printf("%-3d %-25s %-15s %-8d %-15.15s %.50s\n", 
               i + 1, 
               results[i].name, 
               results[i].version, 
               results[i].votes,
               results[i].maintainer ? results[i].maintainer : "None", 
               results[i].description);
    }
    printf("\n");
}

// Create necessary directories
void create_directories() {
    system("mkdir -p " TMP_DIR);
}

// Download and build package
int download_and_build_package(const char *package_name) {
    char command[MAX_BUFFER];
    int status;
    
    // Get into temp directory
    chdir(TMP_DIR);
    
    // Download package
    snprintf(command, MAX_BUFFER, "curl -s %s%s.tar.gz -o %s.tar.gz", AUR_PKG_URL, package_name, package_name);
    status = system(command);
    if(status != 0) {
        fprintf(stderr, "Error: Failed to download package %s\n", package_name);
        return 1;
    }
    
    // Extract package
    snprintf(command, MAX_BUFFER, "tar -xzf %s.tar.gz", package_name);
    status = system(command);
    if(status != 0) {
        fprintf(stderr, "Error: Failed to extract package %s\n", package_name);
        return 1;
    }
    
    // Build package
    snprintf(command, MAX_BUFFER, "cd %s && makepkg -si --noconfirm", package_name);
    status = system(command);
    if(status != 0) {
        fprintf(stderr, "Error: Failed to build/install package %s\n", package_name);
        return 1;
    }
    
    // Clean up
    snprintf(command, MAX_BUFFER, "rm -rf %s*", package_name);
    system(command);
    
    return 0;
}

// Install package
int install_package(const char *package_name) {
    printf("Installing %s...\n", package_name);
    
    // Check if package exists in official repositories
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "pacman -Si %s > /dev/null 2>&1", package_name);
    
    if(system(command) == 0) {
        printf("Package %s found in official repositories. Installing with pacman...\n", package_name);
        snprintf(command, MAX_BUFFER, "sudo pacman -S --noconfirm %s", package_name);
        return system(command);
    } else {
        printf("Package %s not found in official repositories. Installing from AUR...\n", package_name);
        return download_and_build_package(package_name);
    }
}

// Remove package
int remove_package(const char *package_name) {
    printf("Removing %s...\n", package_name);
    
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "sudo pacman -R %s", package_name);
    
    return system(command);
}

// Free package data
void free_package_data(Package *packages, int count) {
    for(int i = 0; i < count; i++) {
        free(packages[i].name);
        free(packages[i].version);
        free(packages[i].description);
        free(packages[i].maintainer);
        free(packages[i].url);
    }
    
    free(packages);
}

// Print usage
void print_usage() {
    printf("Usage: methaur [options] [package]\n");
    printf("Options:\n");
    printf("  -S, --sync       Search and install package (default action)\n");
    printf("  -R, --remove     Remove package\n");
    printf("  -h, --help       Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  methaur firefox     Search and choose firefox packages to install\n");
    printf("  methaur -S firefox  Same as above\n");
    printf("  methaur -R firefox  Remove firefox package\n");
}

int main(int argc, char *argv[]) {
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create necessary directories
    create_directories();
    
    // Check arguments
    if(argc < 2) {
        print_usage();
        return 1;
    }
    
    // Parse arguments
    if(strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    } else if(strcmp(argv[1], "-R") == 0 || strcmp(argv[1], "--remove") == 0) {
        if(argc < 3) {
            fprintf(stderr, "Error: No package specified for removal\n");
            return 1;
        }
        return remove_package(argv[2]);
    } else {
        const char *query;
        
        // Handle -S flag
        if(strcmp(argv[1], "-S") == 0 || strcmp(argv[1], "--sync") == 0) {
            if(argc < 3) {
                fprintf(stderr, "Error: No package specified for installation\n");
                return 1;
            }
            query = argv[2];
        } else {
            query = argv[1];
        }
        
        // Search for packages
        Package *results;
        int count;
        
        search_packages(query, &results, &count);
        
        if(count == 0) {
            printf("No packages found for '%s'\n", query);
            return 1;
        }
        
        display_search_results(results, count);
        
        // Ask for selection
        int selection;
        printf("Enter package number to install (1-%d), or 0 to cancel: ", count);
        scanf("%d", &selection);
        
        if(selection <= 0 || selection > count) {
            printf("Installation cancelled.\n");
            free_package_data(results, count);
            return 0;
        }
        
        int ret = install_package(results[selection - 1].name);
        free_package_data(results, count);
        
        return ret;
    }
    
    // Clean up curl
    curl_global_cleanup();
    
    return 0;
}
