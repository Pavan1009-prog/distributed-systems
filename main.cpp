#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <memory>
#include <filesystem>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <sqlite3.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

// Configuration constants
const size_t CHUNK_SIZE = 10 * 1024 * 1024; // 10MB chunks
const int AES_KEY_SIZE = 256;
const int NUM_UPLOAD_THREADS = 4;

// Encryption utility class
class Encryption {
private:
    unsigned char key[32]; // 256-bit key
    unsigned char iv[16];  // 128-bit IV

public:
    Encryption() {
        // Generate random key and IV
        RAND_bytes(key, sizeof(key));
        RAND_bytes(iv, sizeof(iv));
    }

    void setKey(const unsigned char* k, const unsigned char* i) {
        memcpy(key, k, 32);
        memcpy(iv, i, 16);
    }

    void getKey(unsigned char* k, unsigned char* i) const {
        memcpy(k, key, 32);
        memcpy(i, iv, 16);
    }

    std::vector<unsigned char> encrypt(const std::vector<unsigned char>& plaintext) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        std::vector<unsigned char> ciphertext(plaintext.size() + AES_BLOCK_SIZE);
        int len, ciphertext_len;

        EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv);
        EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), plaintext.size());
        ciphertext_len = len;
        EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
        ciphertext_len += len;

        EVP_CIPHER_CTX_free(ctx);
        ciphertext.resize(ciphertext_len);
        return ciphertext;
    }

    std::vector<unsigned char> decrypt(const std::vector<unsigned char>& ciphertext) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        std::vector<unsigned char> plaintext(ciphertext.size());
        int len, plaintext_len;

        EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv);
        EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size());
        plaintext_len = len;
        EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
        plaintext_len += len;

        EVP_CIPHER_CTX_free(ctx);
        plaintext.resize(plaintext_len);
        return plaintext;
    }
};

// Database manager
class DatabaseManager {
private:
    sqlite3* db;
    std::mutex db_mutex;

public:
    DatabaseManager(const std::string& db_path) {
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Cannot open database");
        }
        initTables();
    }

    ~DatabaseManager() {
        sqlite3_close(db);
    }

    void initTables() {
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS files (
                file_id INTEGER PRIMARY KEY AUTOINCREMENT,
                original_path TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                chunk_count INTEGER NOT NULL,
                encryption_key BLOB NOT NULL,
                encryption_iv BLOB NOT NULL,
                backup_date TEXT NOT NULL,
                status TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS chunks (
                chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,
                file_id INTEGER NOT NULL,
                chunk_index INTEGER NOT NULL,
                chunk_size INTEGER NOT NULL,
                cloud_provider TEXT NOT NULL,
                remote_path TEXT NOT NULL,
                checksum TEXT NOT NULL,
                upload_status TEXT NOT NULL,
                FOREIGN KEY (file_id) REFERENCES files(file_id)
            );
        )";

        char* err_msg = nullptr;
        std::lock_guard<std::mutex> lock(db_mutex);
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string error(err_msg);
            sqlite3_free(err_msg);
            throw std::runtime_error("SQL error: " + error);
        }
    }

    int insertFile(const std::string& path, size_t size, int chunk_count,
                   const unsigned char* key, const unsigned char* iv) {
        std::lock_guard<std::mutex> lock(db_mutex);
        
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");

        sqlite3_stmt* stmt;
        const char* sql = R"(
            INSERT INTO files (original_path, file_size, chunk_count, 
                             encryption_key, encryption_iv, backup_date, status)
            VALUES (?, ?, ?, ?, ?, ?, 'pending')
        )";

        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, size);
        sqlite3_bind_int(stmt, 3, chunk_count);
        sqlite3_bind_blob(stmt, 4, key, 32, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 5, iv, 16, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, ss.str().c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        int file_id = sqlite3_last_insert_rowid(db);
        sqlite3_finalize(stmt);

        return file_id;
    }

    void insertChunk(int file_id, int chunk_index, size_t chunk_size,
                    const std::string& provider, const std::string& remote_path,
                    const std::string& checksum) {
        std::lock_guard<std::mutex> lock(db_mutex);

        sqlite3_stmt* stmt;
        const char* sql = R"(
            INSERT INTO chunks (file_id, chunk_index, chunk_size, 
                              cloud_provider, remote_path, checksum, upload_status)
            VALUES (?, ?, ?, ?, ?, ?, 'uploaded')
        )";

        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, file_id);
        sqlite3_bind_int(stmt, 2, chunk_index);
        sqlite3_bind_int64(stmt, 3, chunk_size);
        sqlite3_bind_text(stmt, 4, provider.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, remote_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, checksum.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void updateFileStatus(int file_id, const std::string& status) {
        std::lock_guard<std::mutex> lock(db_mutex);

        sqlite3_stmt* stmt;
        const char* sql = "UPDATE files SET status = ? WHERE file_id = ?";

        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, file_id);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
};

// Cloud provider interface (simulated)
class CloudProvider {
private:
    std::string name;
    std::string base_path;

public:
    CloudProvider(const std::string& n, const std::string& path) 
        : name(n), base_path(path) {
        fs::create_directories(base_path);
    }

    bool upload(const std::vector<unsigned char>& data, const std::string& filename) {
        try {
            std::string full_path = base_path + "/" + filename;
            std::ofstream file(full_path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            file.close();
            
            // Simulate network delay
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return true;
        } catch (...) {
            return false;
        }
    }

    std::vector<unsigned char> download(const std::string& filename) {
        std::string full_path = base_path + "/" + filename;
        std::ifstream file(full_path, std::ios::binary);
        
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<unsigned char> data(size);
        file.read(reinterpret_cast<char*>(data.data()), size);
        file.close();

        return data;
    }

    std::string getName() const { return name; }
};

// Main backup system
class BackupSystem {
private:
    std::unique_ptr<DatabaseManager> db;
    std::vector<std::unique_ptr<CloudProvider>> providers;
    std::mutex queue_mutex;
    std::queue<std::function<void()>> upload_queue;
    std::vector<std::thread> worker_threads;
    bool stop_workers;

    struct ChunkInfo {
        int index;
        std::vector<unsigned char> data;
        std::string provider_name;
        std::string remote_path;
    };

public:
    BackupSystem(const std::string& db_path) : stop_workers(false) {
        db = std::make_unique<DatabaseManager>(db_path);
        
        // Initialize cloud providers (simulated with local directories)
        providers.push_back(std::make_unique<CloudProvider>("GoogleDrive", "./backup/gdrive"));
        providers.push_back(std::make_unique<CloudProvider>("Dropbox", "./backup/dropbox"));
        providers.push_back(std::make_unique<CloudProvider>("OneDrive", "./backup/onedrive"));

        // Start worker threads
        for (int i = 0; i < NUM_UPLOAD_THREADS; ++i) {
            worker_threads.emplace_back(&BackupSystem::workerThread, this);
        }
    }

    ~BackupSystem() {
        stop_workers = true;
        for (auto& thread : worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void workerThread() {
        while (!stop_workers) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!upload_queue.empty()) {
                    task = upload_queue.front();
                    upload_queue.pop();
                }
            }

            if (task) {
                task();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    std::string calculateChecksum(const std::vector<unsigned char>& data) {
        // Simple checksum (in production, use SHA-256)
        size_t sum = 0;
        for (auto byte : data) {
            sum += byte;
        }
        std::stringstream ss;
        ss << std::hex << sum;
        return ss.str();
    }

    void backupFile(const std::string& filepath) {
        std::cout << "Starting backup of: " << filepath << std::endl;

        // Read file
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open file: " + filepath);
        }

        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // Calculate number of chunks
        int chunk_count = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        std::cout << "File size: " << file_size << " bytes" << std::endl;
        std::cout << "Will create " << chunk_count << " chunks" << std::endl;

        // Create encryption object
        Encryption enc;
        unsigned char key[32], iv[16];
        enc.getKey(key, iv);

        // Insert file record
        int file_id = db->insertFile(filepath, file_size, chunk_count, key, iv);

        // Split, encrypt, and distribute chunks
        for (int i = 0; i < chunk_count; ++i) {
            size_t chunk_size = std::min(CHUNK_SIZE, file_size - (i * CHUNK_SIZE));
            std::vector<unsigned char> chunk(chunk_size);
            file.read(reinterpret_cast<char*>(chunk.data()), chunk_size);

            // Encrypt chunk
            std::vector<unsigned char> encrypted = enc.encrypt(chunk);

            // Select provider (round-robin)
            auto& provider = providers[i % providers.size()];
            std::string remote_filename = "file_" + std::to_string(file_id) + 
                                         "_chunk_" + std::to_string(i) + ".enc";

            // Calculate checksum
            std::string checksum = calculateChecksum(encrypted);

            // Queue upload task
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                upload_queue.push([this, provider = provider.get(), encrypted, 
                                  remote_filename, file_id, i, chunk_size, checksum]() {
                    std::cout << "Uploading chunk " << i << " to " 
                             << provider->getName() << std::endl;
                    
                    if (provider->upload(encrypted, remote_filename)) {
                        db->insertChunk(file_id, i, chunk_size, 
                                      provider->getName(), remote_filename, checksum);
                        std::cout << "Chunk " << i << " uploaded successfully" << std::endl;
                    } else {
                        std::cerr << "Failed to upload chunk " << i << std::endl;
                    }
                });
            }
        }

        file.close();

        // Wait for all uploads to complete
        while (true) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (upload_queue.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        db->updateFileStatus(file_id, "completed");
        std::cout << "Backup completed successfully!" << std::endl;
    }
};

int main() {
    try {
        std::cout << "=== Distributed File Backup System ===" << std::endl;
        std::cout << "Initializing..." << std::endl;

        BackupSystem system("backup.db");

        // Create a test file
        std::string test_file = "test_data.bin";
        std::cout << "\nCreating test file (50MB)..." << std::endl;
        {
            std::ofstream file(test_file, std::ios::binary);
            std::vector<char> data(50 * 1024 * 1024); // 50MB
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = rand() % 256;
            }
            file.write(data.data(), data.size());
        }

        // Backup the file
        std::cout << "\nStarting backup process..." << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        
        system.backupFile(test_file);
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
        
        std::cout << "\n=== Backup Summary ===" << std::endl;
        std::cout << "Time taken: " << duration.count() << " seconds" << std::endl;
        std::cout << "Check the './backup' directory for distributed chunks" << std::endl;
        std::cout << "Check 'backup.db' for metadata" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
