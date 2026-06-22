#include <iostream>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

// Advanced DBMS Lab 1
// File I/O, System Calls, and the Storage Journey
// This program demonstrates standard C/POSIX file I/O that can be traced with strace.

int main() {
    const char* file_name = "assignment-data.txt";
    const char* content = "Advanced DBMS Lab 1\n"
                          "This file was written using POSIX open/write and read using open/read.\n"
                          "It demonstrates how user code reaches the operating system through system calls.\n";

    // 1. Write to file
    // O_WRONLY: Write only
    // O_CREAT: Create file if it doesn't exist
    // O_TRUNC: Truncate file to zero length if it exists
    // 0666: Read/Write permissions
    int fd_out = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_out < 0) {
        std::cerr << "Failed to open file for writing." << std::endl;
        return 1;
    }

    // Write content directly to kernel
    ssize_t bytes_written = write(fd_out, content, strlen(content));
    if (bytes_written < 0) {
        std::cerr << "Failed to write to file." << std::endl;
        close(fd_out);
        return 1;
    }

    // Ensure data reaches disk (or at least page cache)
    fsync(fd_out);
    close(fd_out);

    std::cout << "File written successfully: " << file_name << "\n\n";

    // 2. Read back from file
    int fd_in = open(file_name, O_RDONLY);
    if (fd_in < 0) {
        std::cerr << "Failed to open file for reading." << std::endl;
        return 1;
    }

    char buffer[1024];
    ssize_t bytes_read = read(fd_in, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        std::cerr << "Failed to read from file." << std::endl;
        close(fd_in);
        return 1;
    }

    buffer[bytes_read] = '\0'; // null terminate
    close(fd_in);

    std::cout << "Read back from file:\n\n";
    std::cout << buffer << std::endl;

    return 0;
}
