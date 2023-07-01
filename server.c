#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define MAX_REQUEST_LENGTH 1024
#define END_OF_REQUEST "\r\n\r\n"
#define NOT_IMPLEMENTED "HTTP/1.1 501 Not Implemented\r\n\r\n"
#define NOT_FOUND "HTTP/1.1 404 Not Found\r\n\r\nPage not found."
#define FILE_RESPONSE "HTTP/1.1 200 OK\r\n\r\n"

typedef struct {
    int connection;
    const char* rootDirectory;
    struct sockaddr_in clientAddress;
} ConnectionData;

void sendResponse(int connection, const char* statusCode, const char* content) {
    char response[MAX_REQUEST_LENGTH];
    sprintf(response, "HTTP/1.1 %s\r\n\r\n%s", statusCode, content);
    write(connection, response, strlen(response));
}

int getRequestedData(int connection, const char* rootDirectory, const char* filePath, const char* method, struct sockaddr_in clientAddress) {
    // Validate and sanitize the file path
    if (strchr(filePath, '/') != NULL || strstr(filePath, "..") != NULL) {
        printf("Invalid file path: %s\n", filePath);
        sendResponse(connection, "400 Bad Request", "Invalid file path.");
        return 1;
    }

    // Build the full path to the requested file
    char fullPath[1024];
    snprintf(fullPath, sizeof(fullPath), "%s%s", rootDirectory, filePath);

    if (strcmp(method, "GET") == 0) {
        // Check if the file exists in the root directory
        FILE* file = fopen(fullPath, "rb");
        if (file == NULL) {
            printf("File not found: %s\n", fullPath);
            sendResponse(connection, "404 Not Found", NOT_FOUND);
            return 1;
        }

        // Read the content of the file
        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        char* fileContent = malloc(fileSize + 1);
        if (fileContent == NULL) {
            printf("Error allocating memory for file content: %s\n", strerror(errno));
            fclose(file);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        fread(fileContent, 1, fileSize, file);
        fileContent[fileSize] = '\0';

        fclose(file);

        // Send the file content as the response
        sendResponse(connection, "200 OK", fileContent);

        free(fileContent);

    } else if (strcmp(method, "PUT") == 0) {
        // Read the content from the client
        char buffer[MAX_REQUEST_LENGTH];
        ssize_t bytesRead;
        FILE* file = fopen(fullPath, "wb");
        if (file == NULL) {
            printf("Error creating file: %s\n", fullPath);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        while ((bytesRead = read(connection, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, bytesRead, file);
        }

        if (bytesRead < 0) {
            printf("Error reading from client: %s\n", strerror(errno));
            fclose(file);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        fclose(file);
        sendResponse(connection, "200 OK", "File uploaded successfully.");

    } else if (strcmp(method, "POST") == 0) {
        // Read the content from the client
        char buffer[MAX_REQUEST_LENGTH];
        ssize_t bytesRead;
        FILE* file = fopen(fullPath, "ab");
        if (file == NULL) {
            printf("Error opening file: %s\n", fullPath);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        while ((bytesRead = read(connection, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, bytesRead, file);
        }

        if (bytesRead < 0) {
            printf("Error reading from client: %s\n", strerror(errno));
            fclose(file);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        fclose(file);
        sendResponse(connection, "200 OK", "File appended successfully.");

    } else if (strcmp(method, "DELETE") == 0) {
        int deleteResult = remove(fullPath);
        if (deleteResult != 0) {
            printf("Error deleting file: %s\n", fullPath);
            sendResponse(connection, "500 Internal Server Error", "An error occurred while processing the request.");
            return 1;
        }

        sendResponse(connection, "200 OK", "File deleted successfully.");

    } else {
        sendResponse(connection, "501 Not Implemented", NOT_IMPLEMENTED);
    }

    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(clientAddress.sin_addr), clientIP, INET_ADDRSTRLEN);
    printf("Client IP: %s, Port: %d\n", clientIP, ntohs(clientAddress.sin_port));
    printf("Method: %s, File Path: %s\n", method, filePath);

    return 0;
}

void* handleConnection(void* data) {
    ConnectionData* connectionData = (ConnectionData*)data;
    int connection = connectionData->connection;
    const char* rootDirectory = connectionData->rootDirectory;
    struct sockaddr_in clientAddress = connectionData->clientAddress;

    char request[MAX_REQUEST_LENGTH];
    int bytesRead = read(connection, request, MAX_REQUEST_LENGTH);
    if (bytesRead < 0) {
        printf("Error reading request: %s\n", strerror(errno));
        sendResponse(connection, "500 Internal Server Error", "An error occurred while reading the request.");
        goto cleanup;
    }

    char* method = strtok(request, " \t\n");
    if (method == NULL) {
        sendResponse(connection, "400 Bad Request", "Invalid request.");
        goto cleanup;
    }

    char* filePath = strtok(NULL, " \t\n");
    if (filePath == NULL) {
        sendResponse(connection, "400 Bad Request", "Invalid request.");
        goto cleanup;
    }

    // Remove the leading slash from the file path
    if (filePath[0] == '/') {
        filePath++;
    }

    if (getRequestedData(connection, rootDirectory, filePath, method, clientAddress) != 0) {
        goto cleanup;
    }

cleanup:
    close(connection);
    free(connectionData);
    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <port> <root_directory>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0) {
        printf("Invalid port number: %s\n", argv[1]);
        return 1;
    }

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        printf("Error creating socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddress.sin_port = htons(port);

    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        printf("Error binding socket: %s\n", strerror(errno));
        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, 10) < 0) {
        printf("Error listening on socket: %s\n", strerror(errno));
        close(serverSocket);
        return 1;
    }

    printf("Server listening on port %d\n", port);

    while (1) {
        struct sockaddr_in clientAddress;
        socklen_t clientAddressLength = sizeof(clientAddress);
        int connection = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressLength);
        if (connection < 0) {
            printf("Error accepting connection: %s\n", strerror(errno));
            continue;
        }

        ConnectionData* connectionData = malloc(sizeof(ConnectionData));
        if (connectionData == NULL) {
            printf("Error allocating memory for connection data: %s\n", strerror(errno));
            close(connection);
            continue;
        }

        connectionData->connection = connection;
        connectionData->rootDirectory = argv[2];
        connectionData->clientAddress = clientAddress;

        pthread_t thread;
        int result = pthread_create(&thread, NULL, handleConnection, (void*)connectionData);
        if (result != 0) {
            printf("Error creating thread: %s\n", strerror(result));
            free(connectionData);
            close(connection);
            continue;
        }

        // Add thread to the thread pool or manage thread creation as per your design
        // ...
    }

    close(serverSocket);
    return 0;
}
