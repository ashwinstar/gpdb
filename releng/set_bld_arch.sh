# Define ARCH values
case "`uname -s`" in

    Darwin)
    BLD_ARCH_HOST=`system_profiler SPSoftwareDataType | grep "System Version:" | awk ' {print tolower($4) tolower($5) $6}' | awk -F. ' {print $1 $2}'`_`uname -p | sed -e s/i386/x86/`
    ;;

    Linux)
    case "`cat /etc/redhat-release`" in
        "Red Hat Enterprise Linux WS release 3 (Taroon Update 4)")
        BLD_ARCH_HOST=rhel4_x86_32
        ;;
        *)
        BLD_ARCH_HOST="rhel`cat /etc/redhat-release | sed -e 's/Red Hat Enterprise Linux WS/RedHat/' | awk '{print $3}' | awk -F. '{print $1}'`_`uname -p | sed -e s/i686/x86_32/`"
        ;;
    esac
    ;;

    SunOS)
    BLD_ARCH_HOST="sol`uname -r | awk -F. ' {print $2}'`_`isainfo | awk ' {print $1}' | sed -e s/amd64/x86_64/ | sed -e s/sparcv9/sparc/`"
    ;;

    *)
    BLD_ARCH_HOST="BLD_ARCH_unknown"
    ;;
esac

echo ${BLD_ARCH_HOST}
