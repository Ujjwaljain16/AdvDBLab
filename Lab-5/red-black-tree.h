#ifndef RBT_H
#define RBT_H

#include <vector>

class RedBlackTree {
public:
    enum class Color { RED, BLACK };

    struct Node {
        int   key;
        Color color;
        Node* left;
        Node* right;
        Node* parent;

        explicit Node(int k)
            : key(k), color(Color::RED),
              left(nullptr), right(nullptr), parent(nullptr) {}
    };

    RedBlackTree();
    ~RedBlackTree();

    void insert(int key);
    bool find(int key) const;
    void remove(int key);
    void print() const;          // level-order BFS dump

private:
    Node* m_root;
    Node* m_nil;                 // black sentinel leaf shared by every empty slot

    // Insert helpers
    void fixInsert(Node* z);

    // Rotation primitives
    void leftRotate(Node* x);
    void rightRotate(Node* y);

    // Delete helpers
    Node* minimum(Node* x) const;
    void  transplant(Node* u, Node* v);
    void  fixDelete(Node* x);

    // Utility
    Node* search(int key) const;
    void  freeTree(Node* n);
};

#endif