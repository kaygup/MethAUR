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
#define AUR_INFO_URL "https://aur.archlinux.org/rpc/?v=5&type=info&arg[]="
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
    char **depends;
    int depends_count;
    char **makedepends;
    int makedepends_count;
} Package;

// Structure for curl data
typedef struct {
    char *data;
    size_t size;
} CurlData;

// Function declarations
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void search_packages(const char *query, Package **results, int *count);
void get_package_details(const char *package_name, Package *package);
void display_search_results(Package *results, int count);
int install_package(const char *package_name);
int remove_package(const char *package_name);
void free_package_data(Package *packages, int count);
int download_and_build_package(const char *package_name);
void create_directories();
void print_usage();
char *safe_strdup(const char *str);
int install_dependencies(Package *package);
int check_package_installed(const char *package_name);
void clean_build_environment();

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

// Get detailed package information from AUR
void get_package_details(const char *package_name, Package *package) {
    CURL *curl;
    CURLcode res;
    CurlData chunk;
    char url[MAX_BUFFER];
    
    if (package == NULL || package_name == NULL) {
        return;
    }
    
    // Initialize package
    package->depends = NULL;
    package->depends_count = 0;
    package->makedepends = NULL;
    package->makedepends_count = 0;
    
    chunk.data = malloc(1);
    if (chunk.data == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return;
    }
    chunk.size = 0;
    chunk.data[0] = '\0';
    
    curl = curl_easy_init();
    if (curl) {
        snprintf(url, MAX_BUFFER, "%s%s", AUR_INFO_URL, package_name);
        
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
        
        struct json_object *root = NULL, *results_obj = NULL, *package_obj = NULL;
        
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
            fprintf(stderr, "No package information found for '%s'\n", package_name);
            json_object_put(root);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        package_obj = json_object_array_get_idx(results_obj, 0);
        if (package_obj == NULL) {
            fprintf(stderr, "Error: Failed to get package information\n");
            json_object_put(root);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return;
        }
        
        // Get dependencies
        struct json_object *depends_obj = NULL, *makedepends_obj = NULL;
        
        if (json_object_object_get_ex(package_obj, "Depends", &depends_obj)) {
            int depends_length = json_object_array_length(depends_obj);
            if (depends_length > 0) {
                package->depends = (char **)malloc(depends_length * sizeof(char *));
                if (package->depends != NULL) {
                    package->depends_count = depends_length;
                    for (int i = 0; i < depends_length; i++) {
                        struct json_object *dep_obj = json_object_array_get_idx(depends_obj, i);
                        package->depends[i] = safe_strdup(json_object_get_string(dep_obj));
                    }
                }
            }
        }
        
        if (json_object_object_get_ex(package_obj, "MakeDepends", &makedepends_obj)) {
            int makedepends_length = json_object_array_length(makedepends_obj);
            if (makedepends_length > 0) {
                package->makedepends = (char **)malloc(makedepends_length * sizeof(char *));
                if (package->makedepends != NULL) {
                    package->makedepends_count = makedepends_length;
                    for (int i = 0; i < makedepends_length; i++) {
                        struct json_object *dep_obj = json_object_array_get_idx(makedepends_obj, i);
                        package->makedepends[i] = safe_strdup(json_object_get_string(dep_obj));
                    }
                }
            }
        }
        
        json_object_put(root);
        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "Error: Failed to initialize curl\n");
    }
    
    free(chunk.data);
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
            (*results)[i].depends = NULL;
            (*results)[i].depends_count = 0;
            (*results)[i].makedepends = NULL;
            (*results)[i].makedepends_count = 0;
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

// Check if a package is already installed
int check_package_installed(const char *package_name) {
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "pacman -Q %s > /dev/null 2>&1", package_name);
    return system(command) == 0;
}

// Clean build environment - install missing base-devel packages
void clean_build_environment() {
    printf("Setting up a clean build environment...\n");
    
    // Check if base-devel is installed, if not install it
    if (system("pacman -Qg base-devel > /dev/null 2>&1") != 0) {
        printf("Installing base-devel group which is required for building packages...\n");
        system("sudo pacman -S --needed --noconfirm base-devel");
    }
    
    // Ensure important tools are installed
    const char *essential_tools[] = {
        "autoconf", "automake", "binutils", "bison", "fakeroot", "file", 
        "findutils", "flex", "gawk", "gcc", "gettext", "grep", "groff", 
        "gzip", "libtool", "m4", "make", "pacman", "patch", "pkgconf", 
        "sed", "sudo", "texinfo", "which"
    };
    
    int missing = 0;
    for (size_t i = 0; i < sizeof(essential_tools) / sizeof(essential_tools[0]); i++) {
        char check_cmd[MAX_BUFFER];
        snprintf(check_cmd, MAX_BUFFER, "pacman -Q %s > /dev/null 2>&1", essential_tools[i]);
        if (system(check_cmd) != 0) {
            if (!missing) {
                printf("Installing missing build tools:\n");
                missing = 1;
            }
            printf("  - %s\n", essential_tools[i]);
            char install_cmd[MAX_BUFFER];
            snprintf(install_cmd, MAX_BUFFER, "sudo pacman -S --needed --noconfirm %s", essential_tools[i]);
            system(install_cmd);
        }
    }
    
    if (!missing) {
        printf("All required build tools are already installed.\n");
    }
}

// Install dependencies for a package
int install_dependencies(Package *package) {
    if (package == NULL) {
        return 0;
    }
    
    int ret = 0;
    
    // Install runtime dependencies
    if (package->depends_count > 0 && package->depends != NULL) {
        printf("Installing dependencies...\n");
        
        for (int i = 0; i < package->depends_count; i++) {
            if (package->depends[i] == NULL || strlen(package->depends[i]) == 0) {
                continue;
            }
            
            // Remove version requirements from dependency string
            char *dep = strdup(package->depends[i]);
            char *p = dep;
            while (*p && *p != '<' && *p != '>' && *p != '=') p++;
            *p = '\0';
            
            // Skip if already installed
            if (check_package_installed(dep)) {
                printf("Dependency %s is already installed.\n", dep);
                free(dep);
                continue;
            }
            
            printf("Installing dependency: %s\n", dep);
            install_package(dep);
            free(dep);
        }
    }
    
    // Install build dependencies
    if (package->makedepends_count > 0 && package->makedepends != NULL) {
        printf("Installing build dependencies...\n");
        
        for (int i = 0; i < package->makedepends_count; i++) {
            if (package->makedepends[i] == NULL || strlen(package->makedepends[i]) == 0) {
                continue;
            }
            
            // Remove version requirements from dependency string
            char *dep = strdup(package->makedepends[i]);
            char *p = dep;
            while (*p && *p != '<' && *p != '>' && *p != '=') p++;
            *p = '\0';
            
            // Skip if already installed
            if (check_package_installed(dep)) {
                printf("Build dependency %s is already installed.\n", dep);
                free(dep);
                continue;
            }
            
            printf("Installing build dependency: %s\n", dep);
            install_package(dep);
            free(dep);
        }
    }
    
    return ret;
}

// Download and build package
int download_and_build_package(const char *package_name) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    char command[MAX_BUFFER];
    int status;
    
    // Check if already installed
    if (check_package_installed(package_name)) {
        printf("Package %s is already installed.\n", package_name);
        char upgrade_input[32];
        printf("Do you want to reinstall/upgrade it? (y/N): ");
        fgets(upgrade_input, sizeof(upgrade_input), stdin);
        if (upgrade_input[0] != 'y' && upgrade_input[0] != 'Y') {
            printf("Skipping installation of %s\n", package_name);
            return 0;
        }
    }
    
    // Clean build environment and ensure all necessary tools are available
    clean_build_environment();
    
    // Get package dependencies
    Package package;
    memset(&package, 0, sizeof(Package));
    package.name = safe_strdup(package_name);
    get_package_details(package_name, &package);
    
    // Install dependencies first
    install_dependencies(&package);
    
    // Get into temp directory
    if (chdir(TMP_DIR) != 0) {
        fprintf(stderr, "Error: Failed to change to directory %s\n", TMP_DIR);
        return 1;
    }
    
    // Clean any previous failed builds
    snprintf(command, MAX_BUFFER, "rm -rf %s%s*", TMP_DIR, package_name);
    system(command);
    
    // Download package
    printf("Downloading %s...\n", package_name);
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
    
    // Check if directory exists
    char package_dir[MAX_BUFFER];
    snprintf(package_dir, MAX_BUFFER, "%s%s", TMP_DIR, package_name);
    if (access(package_dir, F_OK) != 0) {
        fprintf(stderr, "Error: Package directory %s not found after extraction\n", package_dir);
        return 1;
    }
    
    // Enter the package directory
    if (chdir(package_dir) != 0) {
        fprintf(stderr, "Error: Failed to change to directory %s\n", package_dir);
        return 1;
    }
    
    // Check if a PKGBUILD exists
    if (access("PKGBUILD", F_OK) != 0) {
        fprintf(stderr, "Error: PKGBUILD file not found\n");
        return 1;
    }
    
    // Special handling for packages with known issues
    if (strcmp(package_name, "cava-git") == 0 || strcmp(package_name, "cava") == 0) {
        printf("Applying special handling for cava packages...\n");
        
        // Ensure required dependencies are installed
        system("sudo pacman -S --needed --noconfirm autoconf automake libtool m4 fftw ncurses alsa-lib iniparser");
        
        // Modify PKGBUILD if necessary to fix common issues
        system("grep -q 'AX_CHECK_GL' PKGBUILD && sed -i 's/--enable-gl/--disable-gl/g' PKGBUILD");
    }
    
    // Build package with better error handling
    printf("Building %s...\n", package_name);
    status = system("makepkg -sf");
    if (status != 0) {
        fprintf(stderr, "Error: Failed to build package %s\n", package_name);
        
        // Try to clean up potential issues and retry
        printf("Trying to fix build issues and retry...\n");
        
        // Check if it's an autotools project and run autoreconf if needed
        if (access("configure.ac", F_OK) == 0 || access("configure.in", F_OK) == 0) {
            printf("Running autoreconf to regenerate build files...\n");
            system("autoreconf -fi");
        }
        
        // Retry build without running configure
        printf("Retrying build...\n");
        status = system("makepkg -sf");
        
        if (status != 0) {
            // If still failing, try with --skipinteg
            printf("Retrying build with --skipinteg...\n");
            status = system("makepkg -sf --skipinteg");
            
            if (status != 0) {
                fprintf(stderr, "Error: Failed to build package %s after multiple attempts\n", package_name);
                
                // Offer to show the build log
                char view_log[32];
                printf("Do you want to view the build log? (y/N): ");
                fgets(view_log, sizeof(view_log), stdin);
                if (view_log[0] == 'y' || view_log[0] == 'Y') {
                    system("cat *.log 2>/dev/null || echo 'No log files found'");
                }
                
                return 1;
            }
        }
    }
    
    // Install package
    printf("Installing %s...\n", package_name);
    snprintf(command, MAX_BUFFER, "sudo pacman -U --noconfirm $(ls %s*.pkg.tar.zst 2>/dev/null || ls %s*.pkg.tar.xz 2>/dev/null || echo 'error')", 
             package_name, package_name);
    status = system(command);
    
    if (status != 0) {
        fprintf(stderr, "Error: Failed to install package %s\n", package_name);
        return 1;
    }
    
    // Clean up
    printf("Cleaning up...\n");
    chdir(TMP_DIR);
    snprintf(command, MAX_BUFFER, "rm -rf %s%s*", TMP_DIR, package_name);
    system(command);
    
    // Free package data
    free(package.name);
    if (package.depends != NULL) {
        for (int i = 0; i < package.depends_count; i++) {
            free(package.depends[i]);
        }
        free(package.depends);
    }
    if (package.makedepends != NULL) {
        for (int i = 0; i < package.makedepends_count; i++) {
            free(package.makedepends[i]);
        }
        free(package.makedepends);
    }
    
    printf("Successfully installed %s\n", package_name);
    return 0;
}

// Install package
int install_package(const char *package_name) {
    if (package_name == NULL || strlen(package_name) == 0) {
        fprintf(stderr, "Error: Invalid package name\n");
        return 1;
    }
    
    printf("Installing %s...\n", package_name);
    
    // Check if package exists in official repositories
    char command[MAX_BUFFER];
    snprintf(command, MAX_BUFFER, "pacman -Si %s > /dev/null 2>&1", package_name);
    
    if (system(command) == 0) {
        printf("Package %s found in official repositories. Installing with pacman...\n", package_name);
        
        // Check for sudo
        if (system("which sudo > /dev/null 2>&1") != 0) {
            fprintf(stderr, "Error: sudo is required but not found\n");
            return 1;
        }
        
        snprintf(command, MAX_BUFFER, "sudo pacman -S --needed --noconfirm %s", package_name);
        return system(command);
    } else {
        printf("Package %s not found in official repositories. Installing from AUR...\n", package_name);
        return download_and_build_package(package_name);
    }
}

// Remove package
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
        
        if (packages[i].depends != NULL) {
            for (int j = 0; j < packages[i].depends_count; j++) {
                free(packages[i].depends[j]);
            }
            free(packages[i].depends);
        }
        
        if (packages[i].makedepends != NULL) {
            for (int j = 0; j < packages[i].makedepends_count; j++) {
                free(packages[i].makedepends[j]);
            }
            free(packages[i].makedepends);
        }
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
    
    // Parse arguments
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
                ret = install_package(results[selection - 1].name);
            }
            
            free_package_data(results, count);
        }
    }
    
    // Clean up curl
    curl_global_cleanup();
    
    return ret;
}
