#include <shared/system.h>
#include <shared/debug.h>
#include "commands.h"
#include "dear_imgui.h"
#include "keys.h"

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static std::map<std::string,CommandTable *> *g_all_command_tables;

static void InitAllCommandTables() {
    if(!g_all_command_tables) {
        static std::map<std::string,CommandTable *> s_all_command_tables;

        g_all_command_tables=&s_all_command_tables;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

Command::Command(std::string name,std::string text):
    m_name(std::move(name)),
    m_text(std::move(text))
{
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const std::string &Command::GetName() const {
    return m_name;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const std::string &Command::GetText() const {
    return m_text;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void CommandTable::ForEachCommandTable(std::function<void(CommandTable *)> fun) {
    InitAllCommandTables();

    for(auto &&it:*g_all_command_tables) {
        fun(it.second);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

CommandTable *CommandTable::FindCommandTableByName(const std::string &name) {
    InitAllCommandTables();

    auto &&it=g_all_command_tables->find(name);
    if(it==g_all_command_tables->end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

CommandTable::CommandTable(std::string name):
    m_keymap(std::move(name),true)
{
    InitAllCommandTables();

    ASSERT(g_all_command_tables->count(this->GetName())==0);
    (*g_all_command_tables)[this->GetName()]=this;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

CommandTable::~CommandTable() {
    ASSERT((*g_all_command_tables)[this->GetName()]==this);
    g_all_command_tables->erase(this->GetName());
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

Command *CommandTable::FindCommandByName(const char *name) const {
    auto &&it=m_command_by_name.find(name);
    if(it!=m_command_by_name.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

Command *CommandTable::FindCommandByName(const std::string &str) const {
    return this->FindCommandByName(str.c_str());
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const std::string &CommandTable::GetName() const {
    return m_keymap.GetName();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void CommandTable::ForEachCommand(std::function<void(Command *)> fun) {
    for(auto &&it:m_command_by_name) {
        fun(it.second.get());
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void CommandTable::ClearMappingsByCommand(Command *command) {
    m_keymap.ClearMappingsByValue(command);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void CommandTable::SetMapping(uint32_t pc_key,Command *command,bool state) {
    ASSERT(this->FindCommandByName(command->m_name));

    m_keymap.SetMapping(pc_key,command,state);

    if(state) {
        if(command->m_shortcut==0) {
            command->m_shortcut=pc_key;
        }
    } else {
        const uint32_t *pc_keys=m_keymap.GetPCKeysForValue(command);
        if(pc_keys) {
            command->m_shortcut=pc_keys[0];
        } else {
            command->m_shortcut=0;
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const uint32_t *CommandTable::GetPCKeysForValue(Command *command) const {
    return m_keymap.GetPCKeysForValue(command);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const Command *const *CommandTable::GetValuesForPCKey(uint32_t key) const {
    return m_keymap.GetValuesForPCKey(key);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

Command *CommandTable::AddCommand(std::unique_ptr<Command> command) {
    ASSERT(!this->FindCommandByName(command->m_name));

    Command *result=command.get();

    m_command_by_name[command->m_name.c_str()]=std::move(command);

    return result;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

CommandContext::CommandContext(void *object,const CommandTable *table):
    m_object(object),
    m_table(table)
{
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void CommandContext::DoMenuItemUI(const char *name,bool enabled) {
    Command *command=m_table->FindCommandByName(name);
    ASSERT(command);

    std::string shortcut;
    if(command->m_shortcut!=0) {
        shortcut=GetKeycodeName(command->m_shortcut).c_str();
    }

    if(ImGui::MenuItem(command->m_text.c_str(),shortcut.empty()?nullptr:shortcut.c_str(),nullptr,enabled)) {
        command->Execute(m_object);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool CommandContext::ExecuteCommandsForPCKey(uint32_t keycode) const {
    const Command *const *commands=m_table->GetValuesForPCKey(keycode);
    if(!commands) {
        return false;
    }

    for(size_t i=0;commands[i];++i) {
        commands[i]->Execute(m_object);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
