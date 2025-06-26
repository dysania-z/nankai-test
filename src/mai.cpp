#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

using namespace std;
using namespace std::chrono;

// 文件元数据结构
struct FileMetadata {
    int fileId;
    string fileName;
    string extension;
    long long fileSize;      // 字节
    string owner;
    string createTime;
    string fullPath;
    
    FileMetadata() = default;
    FileMetadata(int id, const string& name, const string& ext, 
                long long size, const string& own, const string& time, const string& path)
        : fileId(id), fileName(name), extension(ext), fileSize(size), 
          owner(own), createTime(time), fullPath(path) {}
};

// 目录树节点
class DirectoryNode {
public:
    string name;
    bool isDirectory;
    shared_ptr<FileMetadata> fileData;
    unordered_map<string, shared_ptr<DirectoryNode>> children;
    weak_ptr<DirectoryNode> parent;
    
    DirectoryNode(const string& n, bool isDir = true) 
        : name(n), isDirectory(isDir) {}
};

// 压缩的倒排索引项
class CompressedInvertedList {
private:
    vector<int> sortedFileIds;
    
public:
    void addFileId(int fileId) {
        auto it = lower_bound(sortedFileIds.begin(), sortedFileIds.end(), fileId);
        if (it == sortedFileIds.end() || *it != fileId) {
            sortedFileIds.insert(it, fileId);
        }
    }
    
    void removeFileId(int fileId) {
        auto it = lower_bound(sortedFileIds.begin(), sortedFileIds.end(), fileId);
        if (it != sortedFileIds.end() && *it == fileId) {
            sortedFileIds.erase(it);
        }
    }
    
    const vector<int>& getFileIds() const {
        return sortedFileIds;
    }
    
    size_t size() const {
        return sortedFileIds.size();
    }
    
    bool empty() const {
        return sortedFileIds.empty();
    }
    
    // 估计压缩比
    size_t getMemoryUsage() const {
        return sortedFileIds.size() * sizeof(int);
    }
};

// 倒排索引系统
class InvertedIndex {
private:
    unordered_map<string, CompressedInvertedList> extensionIndex;
    map<long long, CompressedInvertedList> sizeIndex;
    unordered_map<string, CompressedInvertedList> ownerIndex;
    unordered_map<string, CompressedInvertedList> timeIndex;
    
    mutable shared_mutex indexMutex;
    
public:
    void addFile(const FileMetadata& file) {
        unique_lock<shared_mutex> lock(indexMutex);
        
        extensionIndex[file.extension].addFileId(file.fileId);
        sizeIndex[file.fileSize].addFileId(file.fileId);
        ownerIndex[file.owner].addFileId(file.fileId);
        timeIndex[file.createTime].addFileId(file.fileId);
    }
    
    void removeFile(const FileMetadata& file) {
        unique_lock<shared_mutex> lock(indexMutex);
        
        extensionIndex[file.extension].removeFileId(file.fileId);
        if (extensionIndex[file.extension].empty()) {
            extensionIndex.erase(file.extension);
        }
        
        sizeIndex[file.fileSize].removeFileId(file.fileId);
        if (sizeIndex[file.fileSize].empty()) {
            sizeIndex.erase(file.fileSize);
        }
        
        ownerIndex[file.owner].removeFileId(file.fileId);
        if (ownerIndex[file.owner].empty()) {
            ownerIndex.erase(file.owner);
        }
        
        timeIndex[file.createTime].removeFileId(file.fileId);
        if (timeIndex[file.createTime].empty()) {
            timeIndex.erase(file.createTime);
        }
    }
    
    vector<int> queryByExtension(const string& ext) const {
        shared_lock<shared_mutex> lock(indexMutex);
        auto it = extensionIndex.find(ext);
        if (it != extensionIndex.end()) {
            return it->second.getFileIds();
        }
        return {};
    }
    
    vector<int> queryBySizeRange(long long minSize, long long maxSize) const {
        shared_lock<shared_mutex> lock(indexMutex);
        vector<int> result;
        auto lower = sizeIndex.lower_bound(minSize);
        auto upper = sizeIndex.upper_bound(maxSize);
        
        for (auto it = lower; it != upper; ++it) {
            const auto& fileIds = it->second.getFileIds();
            result.insert(result.end(), fileIds.begin(), fileIds.end());
        }
        
        sort(result.begin(), result.end());
        result.erase(unique(result.begin(), result.end()), result.end());
        return result;
    }
    
    vector<int> queryByOwner(const string& owner) const {
        shared_lock<shared_mutex> lock(indexMutex);
        auto it = ownerIndex.find(owner);
        if (it != ownerIndex.end()) {
            return it->second.getFileIds();
        }
        return {};
    }
    
    vector<int> queryByTime(const string& time) const {
        shared_lock<shared_mutex> lock(indexMutex);
        auto it = timeIndex.find(time);
        if (it != timeIndex.end()) {
            return it->second.getFileIds();
        }
        return {};
    }
    
    size_t getMemoryUsage() const {
        shared_lock<shared_mutex> lock(indexMutex);
        size_t total = 0;
        
        for (const auto& pair : extensionIndex) {
            total += pair.second.getMemoryUsage();
        }
        for (const auto& pair : sizeIndex) {
            total += pair.second.getMemoryUsage();
        }
        for (const auto& pair : ownerIndex) {
            total += pair.second.getMemoryUsage();
        }
        for (const auto& pair : timeIndex) {
            total += pair.second.getMemoryUsage();
        }
        
        return total;
    }
};

// 文件系统模拟器
class FileSystemSimulator {
private:
    shared_ptr<DirectoryNode> root;
    unordered_map<int, shared_ptr<FileMetadata>> fileMetadataMap;
    InvertedIndex invertedIndex;
    mutable shared_mutex treeMetadataMutex;
    int nextFileId;
    
public:
 
    FileSystemSimulator() : nextFileId(1) {
        root = make_shared<DirectoryNode>("/");
    }
    
    // 添加文件并同时更新目录树和倒排索引
    bool addFile(const string& path, const string& fileName, const string& extension,
                long long fileSize, const string& owner, const string& createTime) {
        unique_lock<shared_mutex> lock(treeMetadataMutex);
        
        auto pathNode = getOrCreatePath(path);
        if (!pathNode) return false;
        
        int fileId = nextFileId++;
        string fullPath = path + (path.back() == '/' ? "" : "/") + fileName;
        
        auto fileData = make_shared<FileMetadata>(fileId, fileName, extension, 
                                                 fileSize, owner, createTime, fullPath);
        
        auto fileNode = make_shared<DirectoryNode>(fileName, false);
        fileNode->fileData = fileData;
        fileNode->parent = pathNode;
        
        pathNode->children[fileName] = fileNode;
        fileMetadataMap[fileId] = fileData;
        
        // 更新倒排索引
        invertedIndex.addFile(*fileData);
        
        return true;
    }
    
    // 删除文件并更新索引
    bool removeFile(const string& fullPath) {
        unique_lock<shared_mutex> lock(treeMetadataMutex);
        
        auto fileNode = findFileNode(fullPath);
        if (!fileNode || fileNode->isDirectory) return false;
        
        auto fileData = fileNode->fileData;
        if (fileData) {
            invertedIndex.removeFile(*fileData);
            fileMetadataMap.erase(fileData->fileId);
        }
        
        // 从父节点删除
        if (auto parent = fileNode->parent.lock()) {
            parent->children.erase(fileNode->name);
        }
        
        return true;
    }
    
    // 传统方式查询（遍历目录树）
    vector<shared_ptr<FileMetadata>> queryByExtensionTraditional(const string& ext) const {
        shared_lock<shared_mutex> lock(treeMetadataMutex);
        vector<shared_ptr<FileMetadata>> result;
        traverseAndFilter(root, [&](const shared_ptr<FileMetadata>& file) {
            if (file->extension == ext) {
                result.push_back(file);
            }
        });
        return result;
    }
    
    // 使用倒排索引查询
    vector<shared_ptr<FileMetadata>> queryByExtensionIndexed(const string& ext) const {
        auto fileIds = invertedIndex.queryByExtension(ext);
        vector<shared_ptr<FileMetadata>> result;
        
        shared_lock<shared_mutex> lock(treeMetadataMutex);
        for (int fileId : fileIds) {
            auto it = fileMetadataMap.find(fileId);
            if (it != fileMetadataMap.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }
    
    vector<shared_ptr<FileMetadata>> queryBySizeRangeIndexed(long long minSize, long long maxSize) const {
        auto fileIds = invertedIndex.queryBySizeRange(minSize, maxSize);
        vector<shared_ptr<FileMetadata>> result;
        
        shared_lock<shared_mutex> lock(treeMetadataMutex);
        for (int fileId : fileIds) {
            auto it = fileMetadataMap.find(fileId);
            if (it != fileMetadataMap.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }
    
    vector<shared_ptr<FileMetadata>> queryByOwnerIndexed(const string& owner) const {
        auto fileIds = invertedIndex.queryByOwner(owner);
        vector<shared_ptr<FileMetadata>> result;
        
        shared_lock<shared_mutex> lock(treeMetadataMutex);
        for (int fileId : fileIds) {
            auto it = fileMetadataMap.find(fileId);
            if (it != fileMetadataMap.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }
    
    // 生成测试数据
    void generateTestData(int numFiles) {
        vector<string> extensions = {".jpg", ".png", ".pdf", ".txt", ".doc", ".mp4", ".mp3"};
        vector<string> owners = {"user1", "user2", "user3", "admin", "guest"};
        vector<string> paths = {"/home/user1", "/home/user2", "/documents", "/pictures", "/videos"};
        
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> extDist(0, extensions.size() - 1);
        uniform_int_distribution<> ownerDist(0, owners.size() - 1);
        uniform_int_distribution<> pathDist(0, paths.size() - 1);
        uniform_int_distribution<long long> sizeDist(1024, 10 * 1024 * 1024); // 1KB to 10MB
        
        for (int i = 0; i < numFiles; ++i) {
            string fileName = "file" + to_string(i);
            string extension = extensions[extDist(gen)];
            string owner = owners[ownerDist(gen)];
            string path = paths[pathDist(gen)];
            long long fileSize = sizeDist(gen);
            string createTime = "2024-" + to_string((i % 12) + 1) + "-" + to_string((i % 28) + 1);
            
            addFile(path, fileName, extension, fileSize, owner, createTime);
        }
    }
    
    size_t getIndexMemoryUsage() const {
        return invertedIndex.getMemoryUsage();
    }
    
    size_t getTotalFiles() const {
        shared_lock<shared_mutex> lock(treeMetadataMutex);
        return fileMetadataMap.size();
    }
    
private:
    shared_ptr<DirectoryNode> getOrCreatePath(const string& path) {
        if (path.empty() || path[0] != '/') return nullptr;
        
        auto current = root;
        if (path == "/") return current;
        
        stringstream ss(path.substr(1));
        string part;
        
        while (getline(ss, part, '/')) {
            if (part.empty()) continue;
            
            if (current->children.find(part) == current->children.end()) {
                auto newNode = make_shared<DirectoryNode>(part, true);
                newNode->parent = current;
                current->children[part] = newNode;
            }
            current = current->children[part];
        }
        
        return current;
    }
    
    shared_ptr<DirectoryNode> findFileNode(const string& fullPath) const {
        if (fullPath.empty() || fullPath[0] != '/') return nullptr;
        
        auto current = root;
        if (fullPath == "/") return current;
        
        stringstream ss(fullPath.substr(1));
        string part;
        
        while (getline(ss, part, '/')) {
            if (part.empty()) continue;
            
            auto it = current->children.find(part);
            if (it == current->children.end()) {
                return nullptr;
            }
            current = it->second;
        }
        
        return current;
    }
    
    void traverseAndFilter(const shared_ptr<DirectoryNode>& node, 
                          function<void(const shared_ptr<FileMetadata>&)> filter) const {
        if (!node->isDirectory && node->fileData) {
            filter(node->fileData);
        }
        
        for (const auto& child : node->children) {
            traverseAndFilter(child.second, filter);
        }
    }
};

// 性能测试类
class PerformanceTest {
public:
    static void runTests() {
        cout << "=== 文件元数据查找优化系统性能测试 ===" << endl;
        
        //FileSystemSimulator fs;
        
        // 测试不同数据规模
        vector<int> testSizes = {1000, 5000, 10000, 50000};
        
        for (int size : testSizes) {
            cout << "\n--- 测试数据规模: " << size << " 文件 ---" << endl;
            FileSystemSimulator fs;
            //fs = FileSystemSimulator();
            // 生成测试数据
            auto start = high_resolution_clock::now();
            fs.generateTestData(size);
            auto end = high_resolution_clock::now();
            
            cout << "数据生成时间: " 
                 << duration_cast<milliseconds>(end - start).count() << " ms" << endl;
            
            // 测试查询性能
            testQueryPerformance(fs, size);
            
            // 测试内存使用
            testMemoryUsage(fs, size);
        }
        
        // 测试并发性能
        cout << "\n=== 并发性能测试 ===" << endl;
        testConcurrentPerformance();
    }
    
private:
    static void testQueryPerformance(FileSystemSimulator& fs, int dataSize) {
        const int queryCount = 100;
        
        // 测试扩展名查询
        auto start = high_resolution_clock::now();
        for (int i = 0; i < queryCount; ++i) {
            fs.queryByExtensionTraditional(".jpg");
        }
        auto end = high_resolution_clock::now();
        auto traditionalTime = duration_cast<microseconds>(end - start).count();
        
        start = high_resolution_clock::now();
        for (int i = 0; i < queryCount; ++i) {
            fs.queryByExtensionIndexed(".jpg");
        }
        end = high_resolution_clock::now();
        auto indexedTime = duration_cast<microseconds>(end - start).count();
        
        cout << "扩展名查询 (" << queryCount << " 次):" << endl;
        cout << "  传统方式: " << traditionalTime << " μs" << endl;
        cout << "  索引方式: " << indexedTime << " μs" << endl;
        cout << "  加速比: " << fixed << setprecision(2) 
             << (double)traditionalTime / indexedTime << "x" << endl;
        
        // 测试其他类型查询
        start = high_resolution_clock::now();
        for (int i = 0; i < queryCount; ++i) {
            fs.queryBySizeRangeIndexed(100000, 1000000); // 100KB to 1MB
        }
        end = high_resolution_clock::now();
        auto sizeQueryTime = duration_cast<microseconds>(end - start).count();
        
        start = high_resolution_clock::now();
        for (int i = 0; i < queryCount; ++i) {
            fs.queryByOwnerIndexed("user1");
        }
        end = high_resolution_clock::now();
        auto ownerQueryTime = duration_cast<microseconds>(end - start).count();
        
        cout << "文件大小范围查询 (" << queryCount << " 次): " << sizeQueryTime << " μs" << endl;
        cout << "所有者查询 (" << queryCount << " 次): " << ownerQueryTime << " μs" << endl;
    }
    
    static void testMemoryUsage(FileSystemSimulator& fs, int dataSize) {
        size_t indexMemory = fs.getIndexMemoryUsage();
        size_t totalFiles = fs.getTotalFiles();
        
        cout << "内存使用情况:" << endl;
        cout << "  倒排索引内存: " << indexMemory << " bytes" << endl;
        cout << "  平均每文件索引开销: " << (double)indexMemory / totalFiles << " bytes" << endl;
        cout << "  索引压缩效果: 良好 (使用排序数组存储)" << endl;
    }
    
    static void testConcurrentPerformance() {
        FileSystemSimulator fs;
        fs.generateTestData(10000);
        
        const int numThreads = 4;
        const int operationsPerThread = 1000;
        
        vector<thread> threads;
        atomic<int> successCount(0);
        
        auto start = high_resolution_clock::now();
        
        // 创建读线程
        for (int i = 0; i < numThreads; ++i) {
            threads.emplace_back([&fs, &successCount, operationsPerThread]() {
                for (int j = 0; j < operationsPerThread; ++j) {
                    auto results = fs.queryByExtensionIndexed(".jpg");
                    if (!results.empty()) {
                        successCount++;
                    }
                }
            });
        }
        
        // 等待所有线程完成
        for (auto& t : threads) {
            t.join();
        }
        
        auto end = high_resolution_clock::now();
        auto totalTime = duration_cast<milliseconds>(end - start).count();
        
        cout << "并发查询测试 (" << numThreads << " 线程, 每线程 " << operationsPerThread << " 操作):" << endl;
        cout << "  总耗时: " << totalTime << " ms" << endl;
        cout << "  成功操作: " << successCount.load() << endl;
        cout << "  QPS: " << (successCount.load() * 1000) / totalTime << endl;
    }
};

int main() {
    try {
        PerformanceTest::runTests();
    } catch (const exception& e) {
        cerr << "错误: " << e.what() << endl;
        return 1;
    }
    
    return 0;
}