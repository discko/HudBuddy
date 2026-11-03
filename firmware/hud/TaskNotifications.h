#pragma once
#include <stdint.h>
#include <vector>

// No task-content diff on the device. Only WFI edges and transient host commands.
template<class Text> class TaskNotifications {
 public:
  struct Result {bool wake=false,chime=false;Text target;};
  struct Entry {Text id;bool wfi=false,red=false;uint32_t changedAt=0;};
  template<class Tasks> Result apply(const Tasks &tasks,const std::vector<Text> &changed,uint32_t now,bool cache=false) {
    Result result;result.wake=!cache && !changed.empty();
    std::vector<Entry> next;
    for(const auto &task:tasks) {
      const Entry *old=find(task.id);
      Entry entry{task.id,task.waitingForInput,old && old->red,old?old->changedAt:0};
      if(cache)entry.red=false;
      else {
        for(const auto &id:changed)if(id==task.id){entry.red=true;entry.changedAt=now;break;}
        if(task.waitingForInput && (!old || !old->wfi)) {
          result.wake=result.chime=true;
          if(result.target==Text())result.target=task.id;
        }
      }
      next.push_back(entry);
    }
    if(!cache && result.target==Text())for(const auto &task:tasks) {
      for(const auto &id:changed)if(task.id==id){result.target=id;break;}
      if(result.target!=Text())break;
    }
    entries_.swap(next);return result;
  }
  bool red(const Text &id,uint32_t now) const {
    auto entry=find(id);return entry && entry->red && uint32_t(now-entry->changedAt)<60000;
  }
  bool expire(uint32_t now) {
    bool changed=false;
    for(auto &entry:entries_)if(entry.red && uint32_t(now-entry.changedAt)>=60000){entry.red=false;changed=true;}
    return changed;
  }
 private:
  const Entry *find(const Text &id) const {for(const auto &e:entries_)if(e.id==id)return &e;return nullptr;}
  std::vector<Entry> entries_;
};
