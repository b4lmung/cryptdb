#include <stdio.h>
#include <string>
#include <iostream>
#include <vector>
#include <map>
#include <assert.h>
using namespace std;

class onion_conf{
    string dir;
    FILE *file;
    char *buf;
    std::map<std::string,std::vector<std::string>> onions_for_num;
    std::map<std::string,std::vector<std::string>> onions_for_str;

    std::vector<std::string> parseline(std::string temp);
    void read_onionlayout_num(std::string temp);
    void read_onionlayout_str(std::string temp);
public:
    std::map<std::string,std::vector<std::string>>& get_onion_levels_num(){return onions_for_num;}
    std::map<std::string,std::vector<std::string>>& get_onion_levels_str(){return onions_for_str;}
    onion_conf(char* filename);
    ~onion_conf();
};

std::vector<std::string> onion_conf::parseline(std::string temp){
    int start = 0;
    int next = 0;
    std::vector<std::string> res;
    while(1){
        char b = temp.back();
        if(b==' '||b=='\n') temp.pop_back();
        else break;
    }
    while((next = temp.find(' ',start))!=-1){
        res.push_back(temp.substr(start,next-start));
        start=next+1;
    }
    res.push_back(temp.substr(start,next-start));
    return res;
}


void onion_conf::read_onionlayout_num(std::string temp){
    std::vector<std::string> res = parseline(temp);
    unsigned int i=1;
    assert(res.size()>1);
    res[0].pop_back();
    std::string onion_name = res[0];
    onions_for_str[onion_name] = std::vector<std::string>();
    for(;i<res.size();i++){
        onions_for_num[onion_name].push_back(res[i]);
    }
}


void onion_conf::read_onionlayout_str(std::string temp){
    std::vector<std::string> res = parseline(temp);
    unsigned int i=1;
    assert(res.size()>1);
    res[0].pop_back();
    std::string onion_name = res[0];
    onions_for_num[onion_name] = std::vector<std::string>();
    for(;i<res.size();i++){
        onions_for_num[onion_name].push_back(res[i]);
    }    

}

onion_conf::onion_conf(char* f=(char*)"onionlayout.conf"){
    file = fopen(f,"r");
    if(file==NULL) {
        printf("error\n");
    }
    buf = (char*)malloc(sizeof(char)*100);
    string current_onion="null";
    size_t n;
    while(getline(&buf,&n,file)!=-1){
        string temp(buf);
        if(temp==string("[onions for num]\n")){
            current_onion = "num";
            continue;
        }else if(temp==string("[onions for str]\n")){
            current_onion = "str";
            continue;
        }else if(temp==string("[end]\n")){
            current_onion = "null";
            continue;
        }else if(temp.size()==0||temp[0]=='#'||temp[0]=='\n'){
            continue;
        }
        if(current_onion==string("num")){
            read_onionlayout_num(temp);
        }else if(current_onion==string("str")){
            read_onionlayout_str(temp);
        }else{
            cout<<"error status:"<<current_onion<<endl;
            return;
        }
    }
    fclose(file);
    free(buf);
}

onion_conf::~onion_conf(){

}

int main(){
    onion_conf of;
    auto res1 = of.get_onion_levels_num();
    auto res2 = of.get_onion_levels_str();

    return 0;
}