
#include <cassert>
#include <sys/types.h>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <linux/securebits.h>

#include <QHostInfo>
#include <QDir>

#include <thread>
#include <future>

#include "filewatcher.h"
#include "fanotify_controller.h"
#include "mount_controller.h"
#include "os.h"
#include "osutil.h"
#include "oscaps.h"
#include "cleanupresource.h"
#include "fdcommunication.h"
#include "util.h"
#include "logger.h"
#include "subprocess.h"
#include "excos.h"
#include "db_globals.h"
#include "db_connection.h"
#include "db_controller.h"
#include "commandinfo.h"
#include "translation.h"
#include "subprocess.h"
#include "app.h"
#include "pathtree.h"
#include "qprocthrow.h"
#include "fileeventhandler.h"
#include "orig_mountspace_process.h"
#include "cpp_exit.h"
#include "qfilethrow.h"
#include "storedfiles.h"
#include "qoutstream.h"
#include "user_str_conversions.h"
#include "socket_message.h"

using socket_message::E_SocketMsg;
using SocketMessages = fdcommunication::SocketCommunication::Messages;
using subprocess::Subprocess;
using osutil::closeVerbose;


static void unshareOrDie(){
    try {
        os::unshare( CLONE_NEWNS);
    } catch (const os::ExcOs& e) {
        logCritical << e.what();
        if(os::geteuid() != 0){
            logCritical << qtr("Note that the effective userid is not 0 (root), so most probably %1 "
                               "does not have the setuid-bit set. As root execute:\n"
                               "chown root %1 && chmod u+s %1").arg(app::SHOURNAL_RUN);
        }
        cpp_exit(1);
    }
}

/// Other applications unsharing their mount-namespace might rely on the
/// fact that they cannot be joined (except from root). Therefor shournal
/// allows only joining of processes whose (effective) gid matches
/// below group.
static gid_t findMsenterGidOrDie(){
    auto* groupInfo = getgrnam(app::MSENTER_ONLY_GROUP);
    if(groupInfo == nullptr){
        logCritical << qtr("group %1 does not exist on your "
                           "system but is required. Please add it:\n"
                           "groupadd %1").arg(app::MSENTER_ONLY_GROUP);
        cpp_exit(1);
    }
    return groupInfo->gr_gid;
}


/// The childprocess's mount-namespace can be joined by shournal-run (msenter).
/// It has a group-id which should be used solely for this purpose which
/// serves as a permission check, so shournal-run cannot be used to join
/// processes which were not 'created' by it.
FileWatcher::MsenterChildReturnValue FileWatcher::setupMsenterTargetChildProcess(){
    assert(os::geteuid() == os::getuid());
    os::seteuid(0);

    // set ids before fork, so parent does not need to wait for child
    // (msenter uid and gid permission check!)
    os::setegid(m_msenterGid);
    os::seteuid(m_realUid);

    auto pipe_ = os::pipe();
    auto msenterPid = os::fork();

    if(msenterPid != 0){
        // parent
        os::seteuid(0);
        os::setegid(os::getgid());
        os::seteuid(m_realUid);
        os::close(pipe_[0]);
        return MsenterChildReturnValue(msenterPid, pipe_[1]);
    }
    // child
    if(m_sockFd != -1){
        // the socket is used to wait for other processes, not this one, so:
        os::close(m_sockFd);
    }
    os::close(pipe_[1]);
    char c;
    // wait unitl parent-process closes its write-end
    os::read(pipe_[0], &c, 1);
    exit(0);
}

FileWatcher::FileWatcher() :
    m_sockFd(-1),
    m_msenterGid(std::numeric_limits<gid_t>::max()),
    m_commandArgc(0),
    m_commandFilename(nullptr),
    m_commandArgv(nullptr),
    m_commandEnvp(environ),
    m_realUid(os::getuid())
{}

void FileWatcher::setupShellLogger()
{
    m_shellLogger.setFullpath(logger::logDir() + "/log_" + app::SHOURNAL + "_shell_integration");
    m_shellLogger.setup();
}


/// Unshare the mount-namespace and mark the interesting mounts with fanotify according
/// to the paths specified in settings.
/// Then either start a new process (passed argv) or wait until the passed socket is closed.
/// In this case, we are in the shell observation mode.
/// To allow other processes to join (--msenter), we fork off a child process with a
/// special group id, which waits for us to finish.
/// Process fanotify events until the observed process finishes (first case) or until
/// all other instances of the passed socket are closed by the observed processes.
/// See also code in directory 'shell-integration'.
void FileWatcher::run()
{
    m_msenterGid = findMsenterGidOrDie();
    orig_mountspace_process::setupIfNotExist();

    unshareOrDie();
    FanotifyController fanotifyCtrl(m_fEventHandler);

    // We process events (filedescriptor-receive- and fanotify-events) with the
    // effective uid of the caller, because read events for files, for which
    // only the owner has read permission, usually fail for
    // root in case of NFS-storages. See also man 5 exports, look for 'root squashing'.
    os::seteuid(m_realUid);
    fanotifyCtrl.setupPaths();

    CommandInfo cmdInfo =  CommandInfo::fromLocalEnv();
    cmdInfo.sessionInfo.uuid = m_shellSessionUUID;

    int ret = 1;
    m_sockCom.setReceiveBufferSize(RECEIVE_BUF_SIZE);
    E_SocketMsg pollResult;
    if(m_commandArgc != 0){
        if(m_commandFilename != nullptr){
            cmdInfo.text += QString(m_commandFilename) + " ";
        }
        cmdInfo.text += argvToQStr(m_commandArgc, m_commandArgv);
        auto sockPair = os::socketpair(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC );
        m_sockCom.setSockFd(sockPair[0]);

        Subprocess proc;
        proc.setAsRealUser(true);
        proc.setEnviron(m_commandEnvp);
        cmdInfo.startTime = QDateTime::currentDateTime();
        // TOODO: evtl. allow to configure proc to not close one of our sockets,
        // to wait on grandchildren.
        // Remove SOCK_CLOEXEC for one of them in that case
        const char* cmdFilename = (m_commandFilename == nullptr) ? m_commandArgv[0]
                                                                 : m_commandFilename;
        proc.call(cmdFilename, m_commandArgv);
        std::future<E_SocketMsg> thread = std::async(&FileWatcher::pollUntilStopped, this,
                                                     std::ref(cmdInfo),
                                                     std::ref(fanotifyCtrl));
        try {
            cmdInfo.returnVal = proc.waitFinish();
        } catch (const os::ExcProcessExitNotNormal& ex) {
            // return typical shell cpp_exit code
            cmdInfo.returnVal = 128 + ex.status();
        }
        ret = cmdInfo.returnVal;
        // that should stop the polling event loop:
        os::close(sockPair[1]);
        thread.wait();
        os::close(sockPair[0]);
        pollResult = thread.get();
    } else if(m_sockFd != -1){
        MsenterChildReturnValue msenterChildRet = setupMsenterTargetChildProcess();
        auto closeMsenterWritePipe = finally([&msenterChildRet] {
            os::close(msenterChildRet.pipeWriteEnd);
            os::waitpid(msenterChildRet.pid);
        });
        m_sockCom.setSockFd(m_sockFd);
        // signal caller, that we're done with setup, if set
        // TODO: find a more accurate way (we start too early in general)
        cmdInfo.startTime = QDateTime::currentDateTime();
        setupShellLogger();
        int rootDirFd = os::open("/", O_RDONLY | O_DIRECTORY);
        auto closeRootDir = finally([&rootDirFd] { closeVerbose(rootDirFd);} );
        SocketMessages sockMesgs;
        m_sockCom.sendMsg({int(E_SocketMsg::SETUP_DONE),
                           qBytesFromVar(msenterChildRet.pid), rootDirFd});

        pollResult = pollUntilStopped(cmdInfo, fanotifyCtrl);
        ret = 0;
    } else {
        pollResult = E_SocketMsg::ENUM_END;
        assert(false);
    }

    cmdInfo.endTime = QDateTime::currentDateTime();

    switch (pollResult) {
    case E_SocketMsg::EMPTY: break; // Normal case
    case E_SocketMsg::ENUM_END:
        logCritical << qtr("Because an error occurred, processing of "
                           "fanotify/socket-events was "
                            "stopped");
        cpp_exit(ret);
    default:
        logWarning << "unhandled case for pollResult: " << int(pollResult);
        break;
    }

    QStringList missingFields;
    if(cmdInfo.text.isEmpty() && cmdInfo.idInDb == db::INVALID_INT_ID){
        // an empty command text should only occur, if the observed shell-session
        // exits. In that case typically only a few file-events occur (e.g. .bash_history)
        // so we have not pushed to database yet (id in db is still invalid).
        // Therefor discard this command.
        logDebug << "command-text is empty, "
                    "not pushing to database...";
        cpp_exit(ret);
    }
    if(cmdInfo.returnVal == CommandInfo::INVALID_RETURN_VAL){
        missingFields += qtr("return value");
    }
    if(! missingFields.isEmpty()){
        logDebug << "The following fields are empty: " << missingFields.join(", ");
    }

    flushToDisk(cmdInfo);
    cpp_exit(ret);
}

void FileWatcher::setShellSessionUUID(const QByteArray &shellSessionUUID)
{
    m_shellSessionUUID = shellSessionUUID;
}

void FileWatcher::setArgv(char **argv, int argc)
{
    m_commandArgv = argv;
    m_commandArgc = argc;
}

void FileWatcher::setCommandEnvp(char **commandEnv)
{
    m_commandEnvp = commandEnv;
}

void FileWatcher::setSockFd(int sockFd)
{
    m_sockFd = sockFd;
}

int FileWatcher::sockFd() const
{
    return m_sockFd;
}

void FileWatcher::setCommandFilename(char *commandFilename)
{
    m_commandFilename = commandFilename;
}


///  @return E_SocketMsg::EMPTY, if processing shall be stopped
E_SocketMsg FileWatcher::processSocketEvent( CommandInfo& cmdInfo ){
    m_sockCom.receiveMessages(&m_sockMessages);
    E_SocketMsg returnMsg = E_SocketMsg::ENUM_END;
    for(auto & msg : m_sockMessages){
        if(msg.bytes.size() > RECEIVE_BUF_SIZE - 1024*10){
            logWarning << "unusual large message received";
        }
        if(msg.msgId == -1){
            return E_SocketMsg::EMPTY;
        }
        assert(msg.msgId >=0 && msg.msgId < int(E_SocketMsg::ENUM_END));

        returnMsg = E_SocketMsg(msg.msgId);

        logDebug << "received message:"
                 << socket_message::socketMsgToStr(E_SocketMsg(msg.msgId));
        switch (E_SocketMsg(msg.msgId)) {
        case E_SocketMsg::COMMAND: {
            cmdInfo.text = msg.bytes;
            break;
        }
        case E_SocketMsg::RETURN_VALUE: {
            cmdInfo.returnVal = varFromQBytes<qint32>(msg.bytes);
            break;
        }
        case E_SocketMsg::LOG_MESSAGE:
            m_shellLogger.stream() << msg.bytes << endl;
            break;

        case E_SocketMsg::CLEAR_EVENTS:
            m_fEventHandler.clearEvents();
            cmdInfo.startTime = QDateTime::currentDateTime();
            break;
        default: {
            // application bug?
            returnMsg = E_SocketMsg::EMPTY;
            logCritical << qtr("invalid message received - : %1").arg(int(msg.msgId));
            break;
        }
        }
    }
    assert(returnMsg != E_SocketMsg::ENUM_END);
    return returnMsg;
}

void FileWatcher::flushToDisk(CommandInfo& cmdInfo){
    assert(os::getegid() == os::getgid());
    assert(os::geteuid() == os::getuid());
    try {
        if(cmdInfo.idInDb == db::INVALID_INT_ID ){
            if(cmdInfo.endTime.isNull()){
                // maybe_todo: create a null-contraint for endTime,
                // which is not straightforward in sqlite.
                // See also: https://stackoverflow.com/questions/4007014/alter-column-in-sqlite
                cmdInfo.endTime = QDateTime::currentDateTime();
            }
            cmdInfo.idInDb = db_controller::addCommand(cmdInfo);
        } else {
            db_controller::updateCommand(cmdInfo);
        }

        StoredFiles::mkpath();
        db_controller::addFileEvents(cmdInfo.idInDb, m_fEventHandler.writeEvents(),
                                     m_fEventHandler.readEvents() );
    } catch (std::exception& e) {
        // May happen, e.g. if we run out of disk space...
        // We discard events anyway, so this error will not happen too soon again...
        logCritical << qtr("Failed to store file-events to disk (they are lost): %1").arg(e.what());
    }
    m_fEventHandler.clearEvents();
}



/// @return: EMPTY, if stopped regulary
///          ENUM_END in case of an error
E_SocketMsg FileWatcher::pollUntilStopped(CommandInfo& cmdInfo,
                             FanotifyController& fanotifyCtrl){
    // At least on centos 7 with Kernel 3.10 CAP_SYS_PTRACE is required, otherwise
    // EACCES occurs on readlink of the received file descriptors
    // Warning: changing euid from 0 to nonzero resets the effective capabilities,
    // so don't do that until processing finished.
    auto caps = os::Capabilites::fromProc();
    const os::Capabilites::CapFlags eventProcessingCaps { CAP_SYS_PTRACE, CAP_SYS_NICE };
    caps->setFlags(CAP_EFFECTIVE, { eventProcessingCaps });
    auto resetEventProcessingCaps = finally([&caps, &eventProcessingCaps] {
        caps->clearFlags(CAP_EFFECTIVE, eventProcessingCaps);
    });

    // slightly increase priority to prevent fanotify queue overflows
    os::setpriority(PRIO_PROCESS, 0, -2);
    auto resetPriority = finally([] {
        os::setpriority(PRIO_PROCESS, 0, 0);
    });

    int poll_num;
    const nfds_t nfds = 2;
    struct pollfd fds[nfds];

    fds[0].fd = m_sockCom.sockFd();
    fds[0].events = POLLIN;

    // Fanotify input
    fds[1].fd = fanotifyCtrl.fanFd();
    fds[1].events = POLLIN;
    while (true) {
        // cleanly cpp_exit poll:
        // poll for two file descriptors: the fanotify descriptor and
        // another one, which receives an cpp_exit-message).
        poll_num = poll(fds, nfds, -1);
        if (poll_num == -1) {
            if (errno == EINTR){     // Interrupted by a signal
                continue;            // Restart poll()
            }
            logCritical << qtr("poll failed (%1) - %2").arg(errno)
                           .arg(translation::strerror_l());
            return E_SocketMsg::ENUM_END;
        }
        // 0 only on timeout, which is infinite
        assert(poll_num != 0);

        // Important: first handle fanotify events, then check the socket if we are done.
        // Otherwise final fanotify-events might get lost!
        if (fds[1].revents & POLLIN) {
            // Fanotify events are available
            fanotifyCtrl.handleEvents();
        }
        if (fds[0].revents & POLLIN) {
            if(processSocketEvent(cmdInfo) == E_SocketMsg::EMPTY){
                return E_SocketMsg::EMPTY;
            }
        }
        auto & prefs = Settings::instance();

        // Note: for a (more or less) short time, the size of cached files might be bigger than
        // specified in settings. That should not be a problem though.
        if(m_fEventHandler.sizeOfCachedReadFiles() >
                prefs.readEventSettings().flushToDiskTotalSize ||
           m_fEventHandler.writeEvents().size() >
                prefs.writeFileSettings().flushToDiskEventCount){
            logInfo << qtr("flushing to disk.");
            flushToDisk(cmdInfo);

        }
    }

}





