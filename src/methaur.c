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
#define ARCH_SEARCH_URL "https://archlinux.org/packages/search/json/?q="
#define TMP_DIR "/tmp/methaur/"

typedef struct {
    char *name;
    char *version;
    char *description;
    int votes;
    char *maintainer;
    char *url;
    char *repo;      
} Package;

typedef struct {
    char *data;
    size_t size;
} CurlData;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void search_aur(const char *query, Package **results, int *count);
void search_arch_repos(const char *query, Package **results, int *count);
void search_packages(const char *query, Package **results, int *count);
void display_search_results(Package *results, int count);
int install_package(const char *package_name, const char *repo);
int remove_package(const char *package_name);
void free_package_data(Package *packages, int count);
int download_and_build_package(const char *package_name);
void create_directories();
void print_usage();
char *safe_strdup(const char *str);

char *safe_strdup(const char *str) {
    return str ? strdup(str) : strdup("");
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    CurlData *mem = (CurlData *)userp;
    
    if (mem == NULL) return 0;
    
    char *ptr = realloc(mem->data, mem->size + real_size + 1);
    if (!ptr) {
        fprintf(stderr, "Error: Out of memory\n");
        return 0;
    }
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = 0;
    
    return real_size;
}

CurlData* init_curl_data() {
    CurlData* chunk = malloc(sizeof(CurlData));
    if (!chunk) return NULL;
    
    chunk->data = malloc(1);
    if (!chunk->data) {
        free(chunk);
        return NULL;
    }
    
    chunk->size = 0;
    chunk->data[0] = '\0';
    return chunk;
}

void search_aur(const char *query, Package **results, int *count) {
    CURL *curl;
    CURLcode res;
    char url[MAX_BUFFER];
    
    CurlData* chunk = init_curl_data();
    if (!chunk) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return;
    }
    
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        free(chunk->data);
        free(chunk);
        return;
    }
    
    snprintf(url, MAX_BUFFER, "%s%s", AUR_RPC_URL, query);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "methaur/1.0");
    
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Error: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    struct json_object *root = NULL, *results_obj = NULL;
    
    enum json_tokener_error jerr = json_tokener_success;
    root = json_tokener_parse_verbose(chunk->data, &jerr);
    
    if (root == NULL || jerr != json_tokener_success) {
        fprintf(stderr, "Error: Failed to parse JSON response\n");
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    if (!json_object_object_get_ex(root, "results", &results_obj) || results_obj == NULL) {
        fprintf(stderr, "Error: No results found in JSON response\n");
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    int num_results = json_object_array_length(results_obj);
    if (num_results <= 0) {
        *count = 0;
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    *count = (num_results > MAX_PACKAGES) ? MAX_PACKAGES : num_results;
    
    *results = (Package *)calloc(*count, sizeof(Package));
    if (*results == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for results\n");
        *count = 0;
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    for (int i = 0; i < *count; i++) {
        struct json_object *package_obj = json_object_array_get_idx(results_obj, i);
        if (package_obj == NULL) continue;
        
        struct json_object *name_obj = NULL, *version_obj = NULL, *desc_obj = NULL;
        struct json_object *votes_obj = NULL, *maintainer_obj = NULL, *url_obj = NULL;
        
        json_object_object_get_ex(package_obj, "Name", &name_obj);
        json_object_object_get_ex(package_obj, "Version", &version_obj);
        json_object_object_get_ex(package_obj, "Description", &desc_obj);
        json_object_object_get_ex(package_obj, "NumVotes", &votes_obj);
        json_object_object_get_ex(package_obj, "Maintainer", &maintainer_obj);
        json_object_object_get_ex(package_obj, "URL", &url_obj);
        
        (*results)[i].name = name_obj ? safe_strdup(json_object_get_string(name_obj)) : safe_strdup("");
        (*results)[i].version = version_obj ? safe_strdup(json_object_get_string(version_obj)) : safe_strdup("");
        (*results)[i].description = desc_obj ? safe_strdup(json_object_get_string(desc_obj)) : safe_strdup("");
        (*results)[i].votes = votes_obj ? json_object_get_int(votes_obj) : 0;
        (*results)[i].maintainer = maintainer_obj ? safe_strdup(json_object_get_string(maintainer_obj)) : safe_strdup("None");
        (*results)[i].url = url_obj ? safe_strdup(json_object_get_string(url_obj)) : safe_strdup("");
        (*results)[i].repo = safe_strdup("aur");
    }
    
    json_object_put(root);
    curl_easy_cleanup(curl);
    free(chunk->data);
    free(chunk);
}

// Search Arch repos
void search_arch_repos(const char *query, Package **results, int *count) {
    CURL *curl;
    CURLcode res;
    char url[MAX_BUFFER];
    
    CurlData* chunk = init_curl_data();
    if (!chunk) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return;
    }
    
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        free(chunk->data);
        free(chunk);
        return;
    }
    
    snprintf(url, MAX_BUFFER, "%s%s", ARCH_SEARCH_URL, query);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "methaur/1.0");
    
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Error: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    struct json_object *root = NULL, *results_obj = NULL;
    
    enum json_tokener_error jerr = json_tokener_success;
    root = json_tokener_parse_verbose(chunk->data, &jerr);
    
    if (root == NULL || jerr != json_tokener_success) {
        fprintf(stderr, "Error: Failed to parse JSON response\n");
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    if (!json_object_object_get_ex(root, "results", &results_obj) || results_obj == NULL) {
        *count = 0;
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    int num_results = json_object_array_length(results_obj);
    if (num_results <= 0) {
        *count = 0;
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    *count = (num_results > MAX_PACKAGES) ? MAX_PACKAGES : num_results;
    
    *results = (Package *)calloc(*count, sizeof(Package));
    if (*results == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for results\n");
        *count = 0;
        json_object_put(root);
        free(chunk->data);
        free(chunk);
        curl_easy_cleanup(curl);
        return;
    }
    
    for (int i = 0; i < *count; i++) {
        struct json_object *package_obj = json_object_array_get_idx(results_obj, i);
        if (package_obj == NULL) continue;
        
        struct json_object *name_obj = NULL, *version_obj = NULL, *desc_obj = NULL;
        struct json_object *repo_obj = NULL, *url_obj = NULL;
        
        json_object_object_get_ex(package_obj, "pkgname", &name_obj);
        json_object_object_get_ex(package_obj, "pkgver", &version_obj);
        json_object_object_get_ex(package_obj, "pkgdesc", &desc_obj);
        json_object_object_get_ex(package_obj, "repo", &repo_obj);
        json_object_object_get_ex(package_obj, "url", &url_obj);
        
        (*results)[i].name = name_obj ? safe_strdup(json_object_get_string(name_obj)) : safe_strdup("");
        (*results)[i].version = version_obj ? safe_strdup(json_object_get_string(version_obj)) : safe_strdup("");
        (*results)[i].description = desc_obj ? safe_strdup(json_object_get_string(desc_obj)) : safe_strdup("");
        (*results)[i].votes = 0; // Official repos don't have votes
        (*results)[i].maintainer = safe_strdup("Arch Linux");
        (*results)[i].url = url_obj ? safe_strdup(json_object_get_string(url_obj)) : safe_strdup("");
        (*results)[i].repo = repo_obj ? safe_strdup(json_object_get_string(repo_obj)) : safe_strdup("unknown");
    }
    
    json_object_put(root);
    curl_easy_cleanup(curl);
    free(chunk->data);
    free(chunk);
}

void search_packages(const char *query, Package **results, int *count) {
    Package *aur_results = NULL;
    Package *arch_results = NULL;
    int aur_count = 0;
    int arch_count = 0;
    
    search_arch_repos(query, &arch_results, &arch_count);
    
    search_aur(query, &aur_results, &aur_count);
    
    int total_count = aur_count + arch_count;
    if (total_count == 0) {
        *count = 0;
        *results = NULL;
        return;
    }
    
    if (total_count > MAX_PACKAGES) {
        total_count = MAX_PACKAGES;
    }
    
    *results = (Package *)calloc(total_count, sizeof(Package));
    if (*results == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for combined results\n");
        free_package_data(aur_results, aur_count);
        free_package_data(arch_results, arch_count);
        *count = 0;
        return;
    }
    
    int result_index = 0;
    for (int i = 0; i < arch_count && result_index < total_count; i++) {
        (*results)[result_index].name = safe_strdup(arch_results[i].name);
        (*results)[result_index].version = safe_strdup(arch_results[i].version);
        (*results)[result_index].description = safe_strdup(arch_results[i].description);
        (*results)[result_index].votes = arch_results[i].votes;
        (*results)[result_index].maintainer = safe_strdup(arch_results[i].maintainer);
        (*results)[result_index].url = safe_strdup(arch_results[i].url);
        (*results)[result_index].repo = safe_strdup(arch_results[i].repo);
        result_index++;
    }
    
    for (int i = 0; i < aur_count && result_index < total_count; i++) {
        (*results)[result_index].name = safe_strdup(aur_results[i].name);
        (*results)[result_index].version = safe_strdup(aur_results[i].version);
        (*results)[result_index].description = safe_strdup(aur_results[i].description);
        (*results)[result_index].votes = aur_results[i].votes;
        (*results)[result_index].maintainer = safe_strdup(aur_results[i].maintainer);
        (*results)[result_index].url = safe_strdup(aur_results[i].url);
        (*results)[result_index].repo = safe_strdup(aur_results[i].repo);
        result_index++;
    }
    
    *count = result_index;
    
    free_package_data(aur_results, aur_count);
    free_package_data(arch_results, arch_count);
}

void display_search_results(Package *results, int count) {
    if (results == NULL || count <= 0) {
        printf("No results to display.\n");
        return;
    }
    
    printf("\n");
    printf("%-3s %-20s %-12s %-8s %-8s %-15s %s\n", "ID", "Name", "Version", "Repo", "Votes", "Maintainer", "Description");
    printf("---------------------------------------------------------------------------------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        printf("%-3d %-20s %-12s %-8s %-8d %-15.15s %.50s\n", 
               i + 1, 
               results[i].name ? results[i].name : "", 
               results[i].version ? results[i].version : "",
               results[i].repo ? results[i].repo : "",
               results[i].votes,
               results[i].maintainer ? results[i].maintainer : "None", 
               results[i].description ? results[i].description : "");
    }
    printf("\n");
}

void create_directories() {
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "mkdir -p %s", TMP_DIR);
    system(command);
}

int download_and_build_package(const char *package_name) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    char command[MAX_BUFFER];
    int status;
    
    if (chdir(TMP_DIR) != 0) {
        fprintf(stderr, "Error: Failed to change to directory %s\n", TMP_DIR);
        return 1;
    }
    
    // Download package
    printf("Downloading %s from AUR...\n", package_name);
    snprintf(command, MAX_BUFFER, "curl -s %s%s.tar.gz -o %s.tar.gz", AUR_PKG_URL, package_name, package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to download package %s\n", package_name);
        return 1;
    }
    
    // Extract package
    printf("Extracting %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "tar -xzf %s.tar.gz", package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to extract package %s\n", package_name);
        return 1;
    }
    
    // Build package
    printf("Building and installing %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "cd %s && makepkg -si --noconfirm", package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to build/install package %s\n", package_name);
        return 1;
    }
    
    // Clean up
    printf("Cleaning up...\n");
    snprintf(command, MAX_BUFFER, "rm -rf %s%s*", TMP_DIR, package_name);
    system(command);
    
    return 0;
}

// Install package
int install_package(const char *package_name, const char *repo) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    printf("Installing %s from %s...\n", package_name, repo);
    
    if (strcmp(repo, "aur") == 0) {
        return download_and_build_package(package_name);
    } else {
        if (system("which sudo > /dev/null 2>&1") != 0) {
            fprintf(stderr, "Error: sudo is required but not found\n");
            return 1;
        }
        
        char command[MAX_BUFFER];
        snprintf(command, MAX_BUFFER, "sudo pacman -S --noconfirm %s", package_name);
        return system(command);
    }
}

int remove_package(const char *package_name) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    printf("Removing %s...\n", package_name);
    
    // Check for sudo
    if (system("which sudo > /dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: sudo is required but not found\n");
        return 1;
    }
    
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "sudo pacman -R --noconfirm %s", package_name);
    
    return system(command);
}

void free_package_data(Package *packages, int count) {
    if (packages == NULL) {
        return;
    }
    
    for (int i = 0; i < count; i++) {
        free(packages[i].name);
        free(packages[i].version);
        free(packages[i].description);
        free(packages[i].maintainer);
        free(packages[i].url);
        free(packages[i].repo);
    }
    
    free(packages);
}

void print_usage() {
    printf("Usage: methaur [options] [package]\n");
    printf("Options:\n");
    printf("  -S, --sync       Search and install package (default action)\n");
    printf("  -R, --remove     Remove package\n");
    printf("  -h, --help       Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  methaur firefox     Search for firefox in official repos and AUR\n");
    printf("  methaur -S firefox  Same as above\n");
    printf("  methaur -R firefox  Remove firefox package\n");
}

int main(int argc, char *argv[]) {
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
  
    create_directories();
    
    
    if (argc < 2) {
        print_usage();
        curl_global_cleanup();
        return 1;
    }
    
    int ret = 0;
    
   
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
    } else if (strcmp(argv[1], "-R") == 0 || strcmp(argv[1], "--remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: No package specified for removal\n");
            ret = 1;
        } else {
            ret = remove_package(argv[2]);
        }
    } else {
        const char *query;
        
        // Handle -S flag
        if (strcmp(argv[1], "-S") == 0 || strcmp(argv[1], "--sync") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Error: No package specified for installation\n");
                curl_global_cleanup();
                return 1;
            }
            query = argv[2];
        } else {
            query = argv[1];
        }
        
        
        Package *results = NULL;
        int count = 0;
        
        search_packages(query, &results, &count);
        
        if (count == 0 || results == NULL) {
            printf("No packages found for '%s'\n", query);
            ret = 1;
        } else {
            display_search_results(results, count);
            
            
            int selection = 0;
            char input[32];
            printf("Enter package number to install (1-%d), or 0 to cancel: ", count);
            if (fgets(input, sizeof(input), stdin) != NULL) {
                selection = atoi(input);
                if (selection < 0) selection = 0;
            }
            
            if (selection <= 0 || selection > count) {
                printf("Installation cancelled.\n");
            } else {
                ret = install_package(results[selection - 1].name, results[selection - 1].repo);
            }
            
            free_package_data(results, count);
        }
    }
    
    
    curl_global_cleanup();
    
    return ret;
}
