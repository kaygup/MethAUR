#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <ctype.h>

#define MAX_PACKAGES 50
#define MAX_BUFFER 8192
#define MAX_DEPS 100
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

// Program options structure
typedef struct {
    int remove_deps;     // Whether to remove build dependencies after installation
    int sync_mode;       // Whether in sync mode
    int remove_mode;     // Whether in remove mode
} Options;

// Function declarations
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void search_packages(const char *query, Package **results, int *count);
void display_search_results(Package *results, int count);
int install_package(const char *package_name, Options *opts);
int remove_package(const char *package_name);
void free_package_data(Package *packages, int count);
int download_and_build_package(const char *package_name, Options *opts);
void create_directories();
void print_usage();
char *safe_strdup(const char *str);
void get_package_dependencies(const char *package_dir, char **deps, int *dep_count);
int install_dependencies(char **deps, int dep_count, Options *opts);

// Safe string duplication (handles NULL)
char *safe_strdup(const char *str) {
    return str ? strdup(str) : strdup("");
}

// Callback function for curl
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    CurlData *mem = (CurlData *)userp;
    
    if (mem == NULL) {
        return 0;
    }
    
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

// Search for packages
void search_packages(const char *query, Package **results, int *count) {
    CURL *curl;
    CURLcode res;
    CurlData chunk;
    char url[MAX_BUFFER];
    
    *count = 0;  // Initialize count to zero in case of failure
    *results = NULL;
    
    chunk.data = malloc(1);
    if (chunk.data == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return;
    }
    chunk.size = 0;
    chunk.data[0] = '\0';
    
    curl = curl_easy_init();
    if (curl) {
        snprintf(url, MAX_BUFFER, "%s%s", AUR_RPC_URL, query);
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "methaur/1.0");
        
        res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            fprintf(stderr, "Error: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        struct json_object *root = NULL, *results_obj = NULL;
        
        // Parse JSON data
        enum json_tokener_error jerr = json_tokener_success;
        root = json_tokener_parse_verbose(chunk.data, &jerr);
        
        if (root == NULL || jerr != json_tokener_success) {
            fprintf(stderr, "Error: Failed to parse JSON response\n");
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        // Get results array
        if (!json_object_object_get_ex(root, "results", &results_obj) || results_obj == NULL) {
            fprintf(stderr, "Error: No results found in JSON response\n");
            json_object_put(root);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        int num_results = json_object_array_length(results_obj);
        if (num_results <= 0) {
            fprintf(stderr, "No packages found for '%s'\n", query);
            json_object_put(root);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        *count = (num_results > MAX_PACKAGES) ? MAX_PACKAGES : num_results;
        
        *results = (Package *)calloc(*count, sizeof(Package));
        if (*results == NULL) {
            fprintf(stderr, "Error: Failed to allocate memory for results\n");
            *count = 0;
            json_object_put(root);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        for (int i = 0; i < *count; i++) {
            struct json_object *package_obj = json_object_array_get_idx(results_obj, i);
            if (package_obj == NULL) {
                continue;
            }
            
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
        }
        
        json_object_put(root);
        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "Error: Failed to initialize curl\n");
    }
    
    free(chunk.data);
}

// Display search results
void display_search_results(Package *results, int count) {
    if (results == NULL || count <= 0) {
        printf("No results to display.\n");
        return;
    }
    
    printf("\n");
    printf("%-3s %-25s %-15s %-8s %-15s %s\n", "ID", "Name", "Version", "Votes", "Maintainer", "Description");
    printf("-----------------------------------------------------------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        printf("%-3d %-25s %-15s %-8d %-15.15s %.50s\n", 
               i + 1, 
               results[i].name ? results[i].name : "", 
               results[i].version ? results[i].version : "", 
               results[i].votes,
               results[i].maintainer ? results[i].maintainer : "None", 
               results[i].description ? results[i].description : "");
    }
    printf("\n");
}

// Create necessary directories
void create_directories() {
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "mkdir -p %s", TMP_DIR);
    system(command);
}

// Parse PKGBUILD for dependencies
void get_package_dependencies(const char *package_dir, char **deps, int *dep_count) {
    char command[MAX_BUFFER];
    char buffer[MAX_BUFFER];
    FILE *fp;
    *dep_count = 0;
    
    // Set up commands to extract dependencies from PKGBUILD
    snprintf(command, MAX_BUFFER, 
             "cd %s && source PKGBUILD && "
             "echo \"${depends[@]}\" && "
             "echo \"${makedepends[@]}\" && "
             "echo \"${checkdepends[@]}\"", 
             package_dir);
    
    fp = popen(command, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: Failed to parse PKGBUILD for dependencies\n");
        return;
    }
    
    // Read dependencies
    while (fgets(buffer, MAX_BUFFER, fp) != NULL && *dep_count < MAX_DEPS) {
        // Skip empty lines
        if (strlen(buffer) <= 1) continue;
        
        // Trim newline
        buffer[strcspn(buffer, "\n")] = 0;
        
        // Split by spaces to get individual dependencies
        char *token = strtok(buffer, " ");
        while (token != NULL && *dep_count < MAX_DEPS) {
            // Skip empty tokens
            if (strlen(token) > 0) {
                // Extract package name (without version constraints)
                char *version_sep = strpbrk(token, "<>=");
                if (version_sep != NULL) {
                    *version_sep = '\0';
                }
                
                // Trim any remaining spaces
                size_t len = strlen(token);
                while (len > 0 && isspace((unsigned char)token[len - 1])) {
                    token[--len] = '\0';
                }
                
                if (strlen(token) > 0) {
                    deps[*dep_count] = strdup(token);
                    (*dep_count)++;
                }
            }
            token = strtok(NULL, " ");
        }
    }
    
    pclose(fp);
}

// Install dependencies
int install_dependencies(char **deps, int dep_count, Options *opts) {
    if (dep_count == 0) {
        return 0; // No dependencies to install
    }
    
    printf("==> Installing %d dependencies...\n", dep_count);
    
    // Track which dependencies are build-only for later removal if requested
    char *build_deps[MAX_DEPS];
    int build_dep_count = 0;
    
    for (int i = 0; i < dep_count; i++) {
        // First try to install from official repos
        char command[MAX_BUFFER];
        snprintf(command, MAX_BUFFER, "pacman -Qi %s >/dev/null 2>&1", deps[i]);
        
        if (system(command) == 0) {
            printf("==> Dependency %s is already installed\n", deps[i]);
            continue;
        }
        
        snprintf(command, MAX_BUFFER, "pacman -Si %s >/dev/null 2>&1", deps[i]);
        
        if (system(command) == 0) {
            printf("==> Installing dependency %s from repositories\n", deps[i]);
            snprintf(command, MAX_BUFFER, "sudo pacman -S --noconfirm --needed %s", deps[i]);
            
            if (system(command) != 0) {
                fprintf(stderr, "Error: Failed to install dependency %s\n", deps[i]);
                return 1;
            }
            
            // Add to list of build deps for potential removal
            if (opts->remove_deps) {
                build_deps[build_dep_count++] = strdup(deps[i]);
            }
        } else {
            // Install from AUR if not in repositories
            printf("==> Installing dependency %s from AUR\n", deps[i]);
            Options dep_opts = *opts;
            dep_opts.remove_deps = 0; // Don't cascade removal for AUR deps
            
            // Recursive dependency installation
            if (install_package(deps[i], &dep_opts) != 0) {
                fprintf(stderr, "Error: Failed to install AUR dependency %s\n", deps[i]);
                return 1;
            }
            
            // Add to list of build deps for potential removal
            if (opts->remove_deps) {
                build_deps[build_dep_count++] = strdup(deps[i]);
            }
        }
    }
    
    // Store build dependencies in a file for later removal if requested
    if (opts->remove_deps && build_dep_count > 0) {
        char filename[MAX_BUFFER];
        snprintf(filename, MAX_BUFFER, "%sbuild_deps", TMP_DIR);
        
        FILE *fp = fopen(filename, "w");
        if (fp != NULL) {
            for (int i = 0; i < build_dep_count; i++) {
                fprintf(fp, "%s\n", build_deps[i]);
                free(build_deps[i]);
            }
            fclose(fp);
        } else {
            fprintf(stderr, "Warning: Could not save build dependency list for later removal\n");
        }
    }
    
    return 0;
}

// Download and build package
int download_and_build_package(const char *package_name, Options *opts) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    char command[MAX_BUFFER];
    char package_dir[MAX_BUFFER];
    int status;
    
    // Get into temp directory
    if (chdir(TMP_DIR) != 0) {
        fprintf(stderr, "Error: Failed to change to directory %s\n", TMP_DIR);
        return 1;
    }
    
    // Download package
    printf("==> Downloading %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "curl -s %s%s.tar.gz -o %s.tar.gz", AUR_PKG_URL, package_name, package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to download package %s\n", package_name);
        return 1;
    }
    
    // Extract package
    printf("==> Extracting %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "tar -xzf %s.tar.gz", package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to extract package %s\n", package_name);
        return 1;
    }
    
    // Set up package directory path
    snprintf(package_dir, MAX_BUFFER, "%s%s", TMP_DIR, package_name);
    
    // Check if directory exists
    if (access(package_dir, F_OK) != 0) {
        fprintf(stderr, "Error: Package directory %s not found after extraction\n", package_dir);
        return 1;
    }
    
    // Parse PKGBUILD for dependencies
    char *deps[MAX_DEPS];
    int dep_count = 0;
    
    printf("==> Parsing PKGBUILD dependencies for %s...\n", package_name);
    get_package_dependencies(package_dir, deps, &dep_count);
    
    if (dep_count > 0) {
        printf("==> Found %d dependencies for %s\n", dep_count, package_name);
        for (int i = 0; i < dep_count; i++) {
            printf("    %s\n", deps[i]);
        }
        
        // Install dependencies
        if (install_dependencies(deps, dep_count, opts) != 0) {
            fprintf(stderr, "Error: Failed to install all dependencies\n");
            
            // Clean up memory
            for (int i = 0; i < dep_count; i++) {
                free(deps[i]);
            }
            
            return 1;
        }
        
        // Clean up memory
        for (int i = 0; i < dep_count; i++) {
            free(deps[i]);
        }
    } else {
        printf("==> No dependencies found for %s\n", package_name);
    }
    
    // Build package
    printf("==> Building and installing %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "cd %s && makepkg -si --noconfirm", package_name);
    status = system(command);
    if (status != 0) {
        fprintf(stderr, "Error: Failed to build/install package %s\n", package_name);
        return 1;
    }
    
    // Clean up
    printf("==> Cleaning up...\n");
    snprintf(command, MAX_BUFFER, "rm -rf %s%s*", TMP_DIR, package_name);
    system(command);
    
    // Remove build dependencies if requested
    if (opts->remove_deps) {
        char dep_file[MAX_BUFFER];
        snprintf(dep_file, MAX_BUFFER, "%sbuild_deps", TMP_DIR);
        
        if (access(dep_file, F_OK) == 0) {
            printf("==> Removing build dependencies...\n");
            
            FILE *fp = fopen(dep_file, "r");
            if (fp != NULL) {
                char line[MAX_BUFFER];
                while (fgets(line, sizeof(line), fp)) {
                    // Trim newline
                    line[strcspn(line, "\n")] = 0;
                    
                    if (strlen(line) > 0) {
                        printf("    Removing %s\n", line);
                        snprintf(command, MAX_BUFFER, "sudo pacman -Rs --noconfirm %s", line);
                        system(command);
                    }
                }
                fclose(fp);
                remove(dep_file);
            }
        }
    }
    
    return 0;
}

// Install package
int install_package(const char *package_name, Options *opts) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    printf("==> Installing %s...\n", package_name);
    
    // Check if package exists in official repositories
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "pacman -Si %s > /dev/null 2>&1", package_name);
    
    if (system(command) == 0) {
        printf("==> Package %s found in official repositories. Installing with pacman...\n", package_name);
        
        // Check for sudo
        if (system("which sudo > /dev/null 2>&1") != 0) {
            fprintf(stderr, "Error: sudo is required but not found\n");
            return 1;
        }
        
        snprintf(command, MAX_BUFFER, "sudo pacman -S --noconfirm --needed %s", package_name);
        return system(command);
    } else {
        printf("==> Package %s not found in official repositories. Installing from AUR...\n", package_name);
        return download_and_build_package(package_name, opts);
    }
}

// Remove package
int remove_package(const char *package_name) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    printf("==> Removing %s...\n", package_name);
    
    // Check for sudo
    if (system("which sudo > /dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: sudo is required but not found\n");
        return 1;
    }
    
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "sudo pacman -R --noconfirm %s", package_name);
    
    return system(command);
}

// Free package data
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
    }
    
    free(packages);
}

// Print usage
void print_usage() {
    printf("Usage: methaur [options] [package]\n");
    printf("Options:\n");
    printf("  -S, --sync       Search and install package (default action)\n");
    printf("  -R, --remove     Remove package\n");
    printf("  -c, --clean      Remove build dependencies after installation\n");
    printf("  -h, --help       Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  methaur firefox          Search and choose firefox packages to install\n");
    printf("  methaur -S firefox       Same as above\n");
    printf("  methaur -S -c firefox    Install firefox and remove build dependencies afterward\n");
    printf("  methaur -R firefox       Remove firefox package\n");
}

int main(int argc, char *argv[]) {
    // Initialize curl
    CURLcode curl_init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init != CURLE_OK) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        return 1;
    }
    
    // Create necessary directories
    create_directories();
    
    // Check arguments
    if (argc < 2) {
        print_usage();
        curl_global_cleanup();
        return 1;
    }
    
    int ret = 0;
    Options opts = {0}; // Initialize all options to 0/false
    
    // Parse arguments
    int arg_index = 1;
    int package_arg_index = -1;
    
    while (arg_index < argc) {
        if (strcmp(argv[arg_index], "-h") == 0 || strcmp(argv[arg_index], "--help") == 0) {
            print_usage();
            curl_global_cleanup();
            return 0;
        } else if (strcmp(argv[arg_index], "-S") == 0 || strcmp(argv[arg_index], "--sync") == 0) {
            opts.sync_mode = 1;
        } else if (strcmp(argv[arg_index], "-R") == 0 || strcmp(argv[arg_index], "--remove") == 0) {
            opts.remove_mode = 1;
        } else if (strcmp(argv[arg_index], "-c") == 0 || strcmp(argv[arg_index], "--clean") == 0) {
            opts.remove_deps = 1;
        } else {
            // First non-option argument is the package name
            if (package_arg_index == -1) {
                package_arg_index = arg_index;
            }
        }
        arg_index++;
    }
    
    // Default to sync mode if no mode specified
    if (!opts.sync_mode && !opts.remove_mode) {
        opts.sync_mode = 1;
    }
    
    // Handle based on mode
    if (opts.remove_mode) {
        if (package_arg_index == -1) {
            fprintf(stderr, "Error: No package specified for removal\n");
            ret = 1;
        } else {
            ret = remove_package(argv[package_arg_index]);
        }
    } else if (opts.sync_mode) {
        const char *query;
        
        if (package_arg_index == -1) {
            fprintf(stderr, "Error: No package specified for installation\n");
            curl_global_cleanup();
            return 1;
        }
        
        query = argv[package_arg_index];
        
        // Search for packages
        Package *results = NULL;
        int count = 0;
        
        search_packages(query, &results, &count);
        
        if (count == 0 || results == NULL) {
            printf("No packages found for '%s'\n", query);
            ret = 1;
        } else {
            display_search_results(results, count);
            
            // Ask for selection
            int selection = 0;
            char input[32];
            printf("Enter package number to install (1-%d), or 0 to cancel: ", count);
            if (fgets(input, sizeof(input), stdin) == NULL) {
                selection = 0;
            } else {
                selection = atoi(input);
                if (selection < 0) selection = 0;
            }
            
            // Handle initial newline if no previous input was read
            if (selection == 0 && input[0] != '0') {
                printf("Enter package number to install (1-%d), or 0 to cancel: ", count);
                if (fgets(input, sizeof(input), stdin) != NULL) {
                    selection = atoi(input);
                    if (selection < 0) selection = 0;
                }
            }
            
            if (selection <= 0 || selection > count) {
                printf("Installation cancelled.\n");
            } else {
                ret = install_package(results[selection - 1].name, &opts);
                
                if (ret == 0) {
                    printf("==> %s has been installed successfully.\n", results[selection - 1].name);
                }
            }
            
            free_package_data(results, count);
        }
    }
    
    // Clean up curl
    curl_global_cleanup();
    
    return ret;
}
