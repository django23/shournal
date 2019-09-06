#pragma once

#include <QString>
#include "util.h"

namespace db_controller {

class QueryColumns {
public:
    static QueryColumns& instance() {
        static QueryColumns s_instance;
        return s_instance;
    }

    const QString cmd_id {"cmd.id"};
    const QString cmd_txt {"cmd.txt"};
    const QString cmd_workingDir {"cmd.workingDirectory"};
    const QString cmd_comment {"cmd.comment"};
    const QString cmd_endtime {"cmd.endTime"};
    const QString cmd_starttime {"cmd.startTime"};
    const QString env_hostname {"env.hostname"};
    const QString env_username {"env.username"};

    const QString rFile_name {"readFile.name"};
    const QString rFile_path {"readFile.path"};
    const QString rFile_mtime {"readFile.mtime"};
    const QString rFile_size {"readFile.size"};

    const QString wFile_name {"writtenFile.name"};
    const QString wfile_mtime {"writtenFile.mtime"};
    const QString wFile_size  {"writtenFile.size"};
    const QString wFile_hash  {"writtenFile.hash"};
    const QString wFile_path  {"writtenFile.path"};

    const QString session_id {"session.id"};
    const QString session_comment {"session.comment"};

private:
    QueryColumns() = default;

public:
    ~QueryColumns() = default;
    Q_DISABLE_COPY(QueryColumns)
    DEFAULT_MOVE(QueryColumns)

};

}
