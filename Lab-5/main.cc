#include "red-black-tree.h"
#include <iostream>

using namespace std;

int main()
{
    RedBlackTree tree;

    int values[] = {50, 25, 75, 10, 30, 60, 80, 5, 15, 70};

    cout << "Inserting: ";
    for (int v : values) {
        cout << v << ' ';
        tree.insert(v);
    }
    cout << "\n\nTree after inserts (level-order, format key:R/B):\n";
    tree.print();

    cout << "\nfind(30)  -> " << (tree.find(30)  ? "found" : "not found") << '\n';
    cout << "find(70)  -> " << (tree.find(70)  ? "found" : "not found") << '\n';
    cout << "find(99)  -> " << (tree.find(99)  ? "found" : "not found") << '\n';

    cout << "\nRemoving 25, 60, 80 ...\n";
    tree.remove(25);
    tree.remove(60);
    tree.remove(80);

    cout << "Tree after removals:\n";
    tree.print();

    cout << "\nfind(25)  -> " << (tree.find(25)  ? "found" : "not found") << '\n';
    cout << "find(50)  -> " << (tree.find(50)  ? "found" : "not found") << '\n';

    return 0;
}