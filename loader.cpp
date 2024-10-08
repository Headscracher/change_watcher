#include <csignal>
#include <iostream>
#include <string>
#include <vector>
#include <sys/inotify.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/select.h>

const int TIMER_PERIOD_MS = 1000;

std::atomic<bool> event_occurred(false);
std::atomic<bool> reset_timer(false);

std::string command;
std::thread command_thread;

std::atomic<bool> stop_flag(false);

pid_t command_pid;

int inotify_fd;
int event_fd;

int watcher_pid;

void timer_function() {
    while (!stop_flag) {
        if (event_occurred) {
            std::this_thread::sleep_for(std::chrono::milliseconds(TIMER_PERIOD_MS));
            if (!reset_timer) {
                std::cout << "Reloading ..." << std::endl;
                if (command_pid > 0) {
                    killpg(command_pid, SIGKILL);
                }
                waitpid(command_pid, NULL, 0);
                command_pid = fork();
                if (command_pid < 0) {
                    std::cerr << "Failed to fork: " << strerror(errno) << std::endl;
                    return;
                } else if (command_pid == 0) {
                    setpgid(0, 0);
                    system(command.c_str());
                    return;
                }
                event_occurred = false;
            } else {
                reset_timer = false;
            }
        }
    }
}


void add_watch_recursive(int inotify_fd, const std::string &directory, const std::vector<std::string> &exclude_paths, std::map<int, std::string> &watch_descriptors) {
    // Skip adding watch if the directory is in the exclude paths
    for (const auto &exclude_path : exclude_paths) {
        if (directory.find(exclude_path) == 0) {
            return;
        }
    }

    int wd = inotify_add_watch(inotify_fd, directory.c_str(), IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        std::cerr << "Failed to add watch for " << directory << ": " << strerror(errno) << std::endl;
        return;
    }

    watch_descriptors[wd] = directory;

    DIR *dir = opendir(directory.c_str());
    if (!dir) {
        std::cerr << "Failed to open directory " << directory << std::endl;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;

            if (name == "." || name == "..")
                continue;

            std::string path = directory + "/" + name;
            add_watch_recursive(inotify_fd, path, exclude_paths, watch_descriptors);
        }
    }

    closedir(dir);
}

void cancel_execution(int signal) {
    stop_flag = true;

    uint64_t u = 1;
    write(event_fd, &u, sizeof(uint64_t));

    if (watcher_pid > 0) {
        kill(watcher_pid, SIGINT);
        waitpid(watcher_pid, nullptr, 0);
    }
    if (command_pid > 0) {
        killpg(command_pid, SIGINT);
        waitpid(command_pid, nullptr, 0);
    }
}

int main(int argc, char *argv[]) 
{
    signal(SIGINT, cancel_execution);
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <directory_to_watch> <command_to_run> [exclude_directory1] [exclude_directory2] ..." << std::endl;
        return 1;
    }
    command = argv[2];
    command_pid = fork();

    if (command_pid < 0) {
        std::cerr << "Failed to fork: " << strerror(errno) << std::endl;
        return 1;
    } else if (command_pid == 0) {
        setpgid(0, 0);
        // Child process
        system(command.c_str());
        return 0;
    } else if (command_pid > 0) {
        inotify_fd = inotify_init();
        if (inotify_fd < 0) {
            std::cerr << "Failed to initialize inotify: " << strerror(errno) << std::endl;
            return 1;
        }

        event_fd = eventfd(0, 0);
        if (event_fd == -1) {
            std::cerr << "Failed to create eventfd: " << strerror(errno) << std::endl;
            close(inotify_fd);
            return 1;
        }

        std::string path_to_watch = argv[1];
        std::vector<std::string> exclude_paths = {};
        for (int i = 3; i < argc; i++) {
            exclude_paths.push_back(argv[i]);
        }

        std::map<int, std::string> watch_descriptors;

        // Add initial watch for the base directory and its subdirectories
        add_watch_recursive(inotify_fd, path_to_watch, exclude_paths, watch_descriptors);

        const size_t event_size = sizeof(struct inotify_event);
        const size_t buffer_size = 1024 * (event_size + 16);
        char buffer[buffer_size];

        watcher_pid = fork();
        if (watcher_pid == 0) {
            std::thread timer_thread(timer_function);
            while (!stop_flag) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(inotify_fd, &fds);
                FD_SET(event_fd, &fds);

                int max_fd = std::max(inotify_fd, event_fd) + 1;

                int ret = select(max_fd, &fds, NULL, NULL, NULL);

                if (ret == -1) {
                    if (errno == EINTR) {
                        continue;
                    } else {
                        std::cerr << "Error during select: " << strerror(errno) << std::endl;
                        break;
                    }
                }

                if (FD_ISSET(inotify_fd, &fds)) {
                    int length = read(inotify_fd, buffer, buffer_size);
                    if (length < 0) {
                        std::cerr << "Error reading from inotify file descriptor: " << strerror(errno) << std::endl;
                        break;
                    }

                    int i = 0;
                    while (i < length) {
                        struct inotify_event *event = (struct inotify_event *)&buffer[i];

                        if (event->len > 0) {
                            std::string event_path = watch_descriptors[event->wd] + "/" + event->name;
                            // Skip events from excluded directories
                            bool exclude = false;
                            for (const auto &exclude_path : exclude_paths) {
                                if (event_path.find(exclude_path) == 0) {
                                    exclude = true;
                                    break;
                                }
                            }

                            if (!exclude) {
                                event_occurred = true;
                                reset_timer = true;
                            }
                        }

                        i += event_size + event->len;
                    }
                }

                if (FD_ISSET(event_fd, &fds)) {
                    break;
                }
                timer_thread.join();
            }
            return 0;
        }
    }
    waitpid(watcher_pid, nullptr, 0);
    close(event_fd);
    close(inotify_fd);
    return 0;
}
