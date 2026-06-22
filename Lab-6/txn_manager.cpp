#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <sstream>

using namespace std;

// Advanced DBMS Lab 6
// Transaction Manager with MVCC Version Chains, Strict 2PL, and Deadlock Detection

enum class LockType { SHARED, EXCLUSIVE };

struct LockRequest {
    int txn_id;
    LockType type;
    bool granted;
};

class LockManager {
private:
    mutex mtx;
    // Map of row_id to a list of lock requests
    unordered_map<int, vector<LockRequest>> lock_table;
    // Map of txn_id to a set of txn_ids it is waiting for
    unordered_map<int, unordered_set<int>> waits_for_graph;
    condition_variable cv;

    bool detect_cycle_dfs(int current, unordered_set<int>& visited, unordered_set<int>& rec_stack) {
        visited.insert(current);
        rec_stack.insert(current);

        for (int neighbor : waits_for_graph[current]) {
            if (rec_stack.count(neighbor)) {
                return true; // Cycle detected
            }
            if (!visited.count(neighbor) && detect_cycle_dfs(neighbor, visited, rec_stack)) {
                return true;
            }
        }
        rec_stack.erase(current);
        return false;
    }

    bool has_deadlock() {
        unordered_set<int> visited;
        unordered_set<int> rec_stack;
        for (const auto& pair : waits_for_graph) {
            if (!visited.count(pair.first)) {
                if (detect_cycle_dfs(pair.first, visited, rec_stack)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool can_grant_lock(int row_id, int txn_id, LockType type) {
        if (lock_table[row_id].empty()) return true;

        for (const auto& req : lock_table[row_id]) {
            if (req.granted && req.txn_id != txn_id) {
                if (type == LockType::EXCLUSIVE || req.type == LockType::EXCLUSIVE) {
                    return false; // Conflict
                }
            }
        }
        return true;
    }

public:
    bool acquire_lock(int txn_id, int row_id, LockType type) {
        unique_lock<mutex> lock(mtx);
        
        while (!can_grant_lock(row_id, txn_id, type)) {
            // Build wait-for graph: txn_id waits for all transactions holding conflicting locks
            for (const auto& req : lock_table[row_id]) {
                if (req.granted && req.txn_id != txn_id) {
                    if (type == LockType::EXCLUSIVE || req.type == LockType::EXCLUSIVE) {
                        waits_for_graph[txn_id].insert(req.txn_id);
                    }
                }
            }

            if (has_deadlock()) {
                // Abort to break deadlock
                waits_for_graph[txn_id].clear();
                return false; 
            }

            // Wait until signaled
            cv.wait(lock);
        }

        // Grant the lock
        waits_for_graph[txn_id].clear();
        
        // Check if already holding a shared lock and upgrading
        bool upgraded = false;
        for (auto& req : lock_table[row_id]) {
            if (req.txn_id == txn_id) {
                if (type == LockType::EXCLUSIVE && req.type == LockType::SHARED) {
                    req.type = LockType::EXCLUSIVE;
                }
                upgraded = true;
                break;
            }
        }

        if (!upgraded) {
            lock_table[row_id].push_back({txn_id, type, true});
        }
        return true;
    }

    void release_locks(int txn_id) {
        unique_lock<mutex> lock(mtx);
        for (auto& pair : lock_table) {
            auto& requests = pair.second;
            requests.erase(remove_if(requests.begin(), requests.end(),
                [txn_id](const LockRequest& r) { return r.txn_id == txn_id; }), requests.end());
        }
        waits_for_graph.erase(txn_id);
        // Remove from other wait-for sets
        for (auto& pair : waits_for_graph) {
            pair.second.erase(txn_id);
        }
        cv.notify_all();
    }
};

struct Version {
    int txn_id;
    string data;
    Version* next;
};

class Database {
private:
    unordered_map<int, Version*> rows;
    LockManager lock_mgr;
    mutex db_mtx; // To protect the row index

public:
    ~Database() {
        for (auto& pair : rows) {
            Version* curr = pair.second;
            while (curr) {
                Version* temp = curr;
                curr = curr->next;
                delete temp;
            }
        }
    }

    bool write(int txn_id, int row_id, const string& data) {
        if (!lock_mgr.acquire_lock(txn_id, row_id, LockType::EXCLUSIVE)) {
            return false; // Deadlock aborted
        }

        lock_guard<mutex> lock(db_mtx);
        // Prepend new version (MVCC)
        Version* new_version = new Version{txn_id, data, rows[row_id]};
        rows[row_id] = new_version;
        return true;
    }

    bool read(int txn_id, int row_id, string& result) {
        if (!lock_mgr.acquire_lock(txn_id, row_id, LockType::SHARED)) {
            return false; // Deadlock aborted
        }

        lock_guard<mutex> lock(db_mtx);
        if (rows.count(row_id) && rows[row_id] != nullptr) {
            // Simply read the latest version (in a real MVCC, we check visibility)
            result = rows[row_id]->data;
            return true;
        }
        result = "NULL";
        return true;
    }

    void commit(int txn_id) {
        // Strict 2PL: Release locks only at commit
        lock_mgr.release_locks(txn_id);
    }

    void abort(int txn_id) {
        // Undo changes (Simplified: removing versions created by this txn)
        lock_guard<mutex> lock(db_mtx);
        for (auto& pair : rows) {
            Version* head = pair.second;
            Version* prev = nullptr;
            while (head) {
                if (head->txn_id == txn_id) {
                    if (prev) prev->next = head->next;
                    else pair.second = head->next;
                    Version* temp = head;
                    head = head->next;
                    delete temp;
                } else {
                    prev = head;
                    head = head->next;
                }
            }
        }
        lock_mgr.release_locks(txn_id);
    }
};

void run_transaction_1(Database& db) {
    int txn_id = 1;
    cout << "Txn 1: Start\n";
    if (!db.write(txn_id, 100, "A=10")) {
        cout << "Txn 1: Aborted due to deadlock\n";
        return;
    }
    this_thread::sleep_for(chrono::milliseconds(100)); // Induce deadlock
    
    if (!db.write(txn_id, 200, "B=20")) {
        cout << "Txn 1: Aborted due to deadlock\n";
        db.abort(txn_id);
        return;
    }
    db.commit(txn_id);
    cout << "Txn 1: Committed\n";
}

void run_transaction_2(Database& db) {
    int txn_id = 2;
    cout << "Txn 2: Start\n";
    if (!db.write(txn_id, 200, "B=30")) {
        cout << "Txn 2: Aborted due to deadlock\n";
        return;
    }
    this_thread::sleep_for(chrono::milliseconds(100)); // Induce deadlock
    
    if (!db.write(txn_id, 100, "A=40")) {
        cout << "Txn 2: Aborted due to deadlock\n";
        db.abort(txn_id);
        return;
    }
    db.commit(txn_id);
    cout << "Txn 2: Committed\n";
}

int main() {
    Database db;
    
    cout << "--- Starting Deadlock Simulation ---\n";
    // Txn 1 writes 100, then 200
    // Txn 2 writes 200, then 100
    // This will cause a classic deadlock. The LockManager will detect the cycle in the waits-for graph and abort one.
    
    thread t1(run_transaction_1, ref(db));
    thread t2(run_transaction_2, ref(db));

    t1.join();
    t2.join();
    
    cout << "--- Final Database State ---\n";
    string res;
    db.read(999, 100, res); // Dummy txn
    cout << "Row 100: " << res << "\n";
    db.read(999, 200, res);
    cout << "Row 200: " << res << "\n";
    db.commit(999);

    return 0;
}
