#include "shell.hpp"
#include <string>

int main(int argc, char* argv[]) {
    std::string db_path = "storage/persistx";
    if (argc > 1) db_path = argv[1];

    Shell shell(db_path);
    shell.run();
    return 0;
}
