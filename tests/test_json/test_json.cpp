#include <rapidjson/document.h>
#include <iostream>

int main() {
    rapidjson::Document d;
    d.Parse("{\"hello\":\"world\"}");
    std::cout << d["hello"].GetString() << std::endl;
    return 0;
}