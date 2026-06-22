#include "red-black-tree.h"
#include <iostream>
#include <queue>
#include <vector>

using namespace std;

/*
 * INSERT FIXUP — after BST-inserting a red node z, fixInsert(z) walks up
 * and repairs any red-red violation using three cases (checked symmetrically
 * for left- and right-leaning parents):
 *
 *   Case 1 — uncle is RED:
 *       Recolor parent + uncle BLACK, grandparent RED, recurse on grandparent.
 *
 *   Case 2 — uncle is BLACK, z is the "inner" grandchild (zigzag):
 *       Rotate around parent to convert the zigzag into a straight line,
 *       then fall through to Case 3.
 *
 *   Case 3 — uncle is BLACK, z is the "outer" grandchild (straight line):
 *       Recolor parent BLACK, grandparent RED, rotate around grandparent.
 *
 * DELETE FIXUP — standard CLRS RBT-Delete.  When a black node is removed,
 * a "double-black" deficit appears at its replacement x.  fixDelete(x)
 * removes the deficit by climbing the tree and applying four sibling cases.
 */

// ─── construction / destruction ──────────────────────────────────────────────

RedBlackTree::RedBlackTree()
    : m_root(nullptr), m_nil(new Node(0))
{
    m_nil->color  = Color::BLACK;
    m_nil->left   = nullptr;
    m_nil->right  = nullptr;
    m_nil->parent = nullptr;
    m_root        = m_nil;
}

RedBlackTree::~RedBlackTree()
{
    freeTree(m_root);
    delete m_nil;
}

void RedBlackTree::freeTree(Node* n)
{
    if (n == m_nil || n == nullptr) return;
    freeTree(n->left);
    freeTree(n->right);
    delete n;
}

// ─── public API ──────────────────────────────────────────────────────────────

bool RedBlackTree::find(int key) const
{
    return search(key) != m_nil;
}

RedBlackTree::Node* RedBlackTree::search(int key) const
{
    Node* cur = m_root;
    while (cur != m_nil) {
        if      (key == cur->key) return cur;
        else if (key  < cur->key) cur = cur->left;
        else                      cur = cur->right;
    }
    return m_nil;
}

void RedBlackTree::insert(int key)
{
    Node* z   = new Node(key);
    z->left   = m_nil;
    z->right  = m_nil;
    z->color  = Color::RED;
    z->parent = nullptr;

    // Standard BST descent to find the insertion point
    Node* parent = nullptr;
    Node* cur    = m_root;
    while (cur != m_nil) {
        parent = cur;
        cur    = (key < cur->key) ? cur->left : cur->right;
    }

    z->parent = parent;
    if      (parent == nullptr)    m_root          = z;
    else if (key < parent->key)    parent->left    = z;
    else                           parent->right   = z;

    fixInsert(z);
    m_root->color = Color::BLACK;   // invariant: root is always black
}

void RedBlackTree::remove(int key)
{
    Node* z = search(key);
    if (z == m_nil) return;          // key not present — no-op

    Node*  y           = z;
    Color  yOrigColor  = y->color;
    Node*  x;                        // node that moves into y's old position

    if (z->left == m_nil) {
        x = z->right;
        transplant(z, z->right);
    } else if (z->right == m_nil) {
        x = z->left;
        transplant(z, z->left);
    } else {
        // z has two real children: replace with its in-order successor
        y          = minimum(z->right);
        yOrigColor = y->color;
        x          = y->right;

        if (y->parent == z) {
            x->parent = y;
        } else {
            transplant(y, y->right);
            y->right         = z->right;
            y->right->parent = y;
        }
        transplant(z, y);
        y->left         = z->left;
        y->left->parent = y;
        y->color        = z->color;
    }

    delete z;

    // A removed black node creates a black-height deficit — fix it
    if (yOrigColor == Color::BLACK)
        fixDelete(x);
}

// ─── rotations ───────────────────────────────────────────────────────────────

/*
 *  leftRotate(x):          rightRotate(y):
 *
 *     x                y         y                x
 *    / \     →        / \       / \     →        / \
 *   a   y            x   c     x   c            a   y
 *      / \          / \       / \                  / \
 *     b   c        a   b     a   b                b   c
 */

void RedBlackTree::leftRotate(Node* x)
{
    Node* y  = x->right;
    x->right = y->left;
    if (y->left != m_nil)
        y->left->parent = x;

    y->parent = x->parent;
    if      (x->parent == nullptr)       m_root           = y;
    else if (x == x->parent->left)       x->parent->left  = y;
    else                                 x->parent->right = y;

    y->left   = x;
    x->parent = y;
}

void RedBlackTree::rightRotate(Node* y)
{
    Node* x  = y->left;
    y->left  = x->right;
    if (x->right != m_nil)
        x->right->parent = y;

    x->parent = y->parent;
    if      (y->parent == nullptr)       m_root           = x;
    else if (y == y->parent->left)       y->parent->left  = x;
    else                                 y->parent->right = x;

    x->right  = y;
    y->parent = x;
}

// ─── insert fixup ────────────────────────────────────────────────────────────

void RedBlackTree::fixInsert(Node* z)
{
    // Loop invariant: z is red and may be creating a red-red violation
    while (z != m_root && z->parent->color == Color::RED) {
        Node* parent      = z->parent;
        Node* grandparent = parent->parent;

        if (parent == grandparent->left) {
            Node* uncle = grandparent->right;

            if (uncle->color == Color::RED) {
                // Case 1 — uncle red: recolor and push violation upward
                parent->color      = Color::BLACK;
                uncle->color       = Color::BLACK;
                grandparent->color = Color::RED;
                z                  = grandparent;
            } else {
                if (z == parent->right) {
                    // Case 2 — LR zigzag: rotate left around parent to get Case 3
                    z           = parent;
                    leftRotate(z);
                    parent      = z->parent;
                    grandparent = parent->parent;
                }
                // Case 3 — LL straight: recolor + rotate right around grandparent
                parent->color      = Color::BLACK;
                grandparent->color = Color::RED;
                rightRotate(grandparent);
            }
        } else {
            // Symmetric: parent is the right child of grandparent
            Node* uncle = grandparent->left;

            if (uncle->color == Color::RED) {
                // Case 1 mirror
                parent->color      = Color::BLACK;
                uncle->color       = Color::BLACK;
                grandparent->color = Color::RED;
                z                  = grandparent;
            } else {
                if (z == parent->left) {
                    // Case 2 mirror — RL zigzag: rotate right around parent
                    z           = parent;
                    rightRotate(z);
                    parent      = z->parent;
                    grandparent = parent->parent;
                }
                // Case 3 mirror — RR straight: recolor + rotate left around grandparent
                parent->color      = Color::BLACK;
                grandparent->color = Color::RED;
                leftRotate(grandparent);
            }
        }
    }
    m_root->color = Color::BLACK;
}

// ─── delete helpers ──────────────────────────────────────────────────────────

RedBlackTree::Node* RedBlackTree::minimum(Node* x) const
{
    while (x->left != m_nil)
        x = x->left;
    return x;
}

// Replace subtree rooted at u with subtree rooted at v
void RedBlackTree::transplant(Node* u, Node* v)
{
    if      (u->parent == nullptr)       m_root           = v;
    else if (u == u->parent->left)       u->parent->left  = v;
    else                                 u->parent->right = v;
    v->parent = u->parent;
}

void RedBlackTree::fixDelete(Node* x)
{
    while (x != m_root && x->color == Color::BLACK) {
        if (x == x->parent->left) {
            Node* sib = x->parent->right;

            // Case 1 — sibling is red: rotate to make sibling black, then recheck
            if (sib->color == Color::RED) {
                sib->color        = Color::BLACK;
                x->parent->color  = Color::RED;
                leftRotate(x->parent);
                sib               = x->parent->right;
            }

            // Case 2 — sibling's both children are black: recolor sibling, move up
            if (sib->left->color == Color::BLACK && sib->right->color == Color::BLACK) {
                sib->color = Color::RED;
                x          = x->parent;
            } else {
                // Case 3 — sibling's right child is black (left is red): fix sibling first
                if (sib->right->color == Color::BLACK) {
                    sib->left->color = Color::BLACK;
                    sib->color       = Color::RED;
                    rightRotate(sib);
                    sib              = x->parent->right;
                }
                // Case 4 — sibling's right child is red: absorb deficit via rotation
                sib->color        = x->parent->color;
                x->parent->color  = Color::BLACK;
                sib->right->color = Color::BLACK;
                leftRotate(x->parent);
                x                 = m_root;           // done
            }
        } else {
            // Symmetric: x is the right child
            Node* sib = x->parent->left;

            if (sib->color == Color::RED) {
                sib->color        = Color::BLACK;
                x->parent->color  = Color::RED;
                rightRotate(x->parent);
                sib               = x->parent->left;
            }

            if (sib->right->color == Color::BLACK && sib->left->color == Color::BLACK) {
                sib->color = Color::RED;
                x          = x->parent;
            } else {
                if (sib->left->color == Color::BLACK) {
                    sib->right->color = Color::BLACK;
                    sib->color        = Color::RED;
                    leftRotate(sib);
                    sib               = x->parent->left;
                }
                sib->color        = x->parent->color;
                x->parent->color  = Color::BLACK;
                sib->left->color  = Color::BLACK;
                rightRotate(x->parent);
                x                 = m_root;
            }
        }
    }
    x->color = Color::BLACK;
}

// ─── BFS print ───────────────────────────────────────────────────────────────

void RedBlackTree::print() const
{
    if (m_root == m_nil) {
        cout << "  (empty tree)\n";
        return;
    }

    // Collect level-order traversal into a vector.
    // Nil children of nil nodes are not enqueued, so the queue always terminates.
    vector<Node*> bfs;
    queue<Node*>  q;
    q.push(m_root);
    while (!q.empty()) {
        Node* cur = q.front(); q.pop();
        bfs.push_back(cur);
        if (cur != m_nil) {
            q.push(cur->left);
            q.push(cur->right);
        }
    }

    // Trim trailing nil sentinels for a cleaner display
    while (!bfs.empty() && bfs.back() == m_nil)
        bfs.pop_back();

    cout << "  [";
    for (int i = 0; i < (int)bfs.size(); ++i) {
        Node* n = bfs[i];
        if (n == m_nil)
            cout << "nil";
        else
            cout << n->key << ":" << (n->color == Color::RED ? "R" : "B");
        if (i + 1 < (int)bfs.size()) cout << ", ";
    }
    cout << "]\n";
}