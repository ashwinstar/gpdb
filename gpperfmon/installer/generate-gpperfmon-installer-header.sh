#!/bin/bash

#
#This script generates a header file for the Greenplum Performance Monitor installer.
#

. ./lib/platform_defines.sh
if [ $? -ne 0 ] ; then
    echo "Error sourcing ./lib/platform_defines.sh"
    exit 1
fi

#$1 needs to be the extension to the install path
#for nightly builds, head or the branch version
#for releases, the version
#i.e.: greenplum-db-head-nightly
#      greenplum-db-2.3-nightly
#      greenplum-db-2.4.0.0
if [ x"$1" = "x" ] ; then
    echo "Error: Version for install location must be specified"
    exit 1
else
    VERSION_EXT="$1"
fi
DEFAULT_INSTALL_PATH="/usr/local/greenplum-perfmon-web-${VERSION_EXT}"

cat <<EOF_HEADER
#!/bin/sh

#Check for needed tools
UTILS="sed tar awk cat mkdir tail mv more"
for util in \${UTILS}; do
    which \${util} > /dev/null 2>&1
    if [ \$? != 0 ] ; then
cat <<EOF
********************************************************************************
Error: \${util} was not found in your path.
       \${util} is needed to run this installer.
       Please add \${util} to your path before running the installer again.
       Exiting installer.
********************************************************************************
EOF
       exit 1
    fi
done

#Verify that tar in path is GNU tar. If not, try using gtar.
#If gtar is not found, exit.
TAR=
tar --version > /dev/null 2>&1
if [ \$? = 0 ] ; then
    TAR=tar
else
    which gtar > /dev/null 2>&1
    if [ \$? = 0 ] ; then
        gtar --version > /dev/null 2>&1
        if [ \$? = 0 ] ; then
            TAR=gtar
        fi
    fi
fi
if [ -z \${TAR} ] ; then
cat <<EOF
********************************************************************************
Error: GNU tar is needed to extract this installer.
       Please add it to your path before running the installer again.
       Exiting installer.
********************************************************************************
EOF
    exit 1
fi
EOF_HEADER

if [ "${MPP_ARCH}" = "OSX-i386" ] ; then
cat << EOF_HEADER
platform=Darwin
arch=i386
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Error: Installer will only install on ${platform} \${arch}"
    exit 1
else
    if [ \`uname -p\` != "\${arch}" ] ; then
        echo "Error: Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi
if [ "${MPP_ARCH}" = "OSX-ppc" ] ; then
cat << EOF_HEADER
platform=Darwin
arch=powerpc
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Error: Installer will only install on \${platform} \${arch}"
    exit 1
else
    if [ \`uname -p\` != "\${arch}" ] ; then
        echo "Error: Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi
if [ "${MPP_ARCH}" = "RHEL4-x86_64" -o "${MPP_ARCH}" = "SuSE10-x86_64" -o "${MPP_ARCH}" = "SuSE11-x86_64" -o "${MPP_ARCH}" = "SuSE11.1-x86_64" ] ; then
cat << EOF_HEADER
platform=Linux
arch=x86_64
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Error: Installer will only install on \${platform} \${arch}"
    exit 1
else
    if [ \`arch\` != "\${arch}" ] ; then
        echo "Error: Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi
if [ "${MPP_ARCH}" = "RHEL4-i386" ] ; then
cat << EOF_HEADER
platform=Linux
arch=i686
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Error: Installer will only install on \${platform} \${arch}"
    exit 1
else
    if [ \`arch\` != "\${arch}" ] ; then
        echo "Error: Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi
if [ "${MPP_ARCH}" = "SOL-x86_64" ] ; then
cat << EOF_HEADER
platform=SunOS
arch=x86_64
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Installer will only install on \${platform} \${arch}"
    exit 1
else
    if \`isalist | grep amd64 > /dev/null 2>&1\` ; then
        guessed_arch=x86_64
    else
        guessed_arch=null
    fi
    if [ "\${guessed_arch}" != "\${arch}" ] ; then
        echo "Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi
if [ "${MPP_ARCH}" = "SOL-sparc" ] ; then
cat << EOF_HEADER
platform=SunOS
arch=sparc
if [ \`uname -s\` != "\${platform}" ] ; then
    echo "Installer will only install on \${platform} \${arch}"
    exit 1
else
    if \`isalist | grep sparcv9 > /dev/null 2>&1\` ; then
        guessed_arch=sparc
    else
        guessed_arch=null
    fi
    if [ "\${guessed_arch}" != "\${arch}" ] ; then
        echo "Installer will only install on \${platform} \${arch}"
        exit 1
    fi
fi
EOF_HEADER
fi

cat <<EOF_HEADER
SKIP=\`awk '/^__END_HEADER__/ {print NR + 1; exit 0; }' \$0\`

more << EOF

********************************************************************************
You must read and accept the Greenplum Database Performance Monitor license 
agreement before installing
********************************************************************************

ACCEPTANCE OF THIS LICENSE AGREEMENT MEANS THAT YOU, OR THE ENTITY THAT YOU
REPRESENT, ARE UNCONDITIONALLY CONSENTING TO BE BOUND BY, AND ARE BECOMING A
PARTY TO, THE LICENSE AGREEMENT CONSISTING OF THIS PARAGRAPH AND THE
FOLLOWING TERMS (THE "AGREEMENT") WITH RESPECT TO THE APPLICABLE GREENPLUM
PRODUCT. DOWNLOAD AND USE OF THE PRODUCT IS CONDITIONED ON ASSENT TO AND
COMPLIANCE WITH THE TERMS OF THIS AGREEMENT TO THE EXCLUSION OF ALL OTHER
TERMS. IF THESE TERMS ARE CONSIDERED AN OFFER, ACCEPTANCE IS EXPRESSLY
LIMITED TO THESE TERMS. IF YOU AND THE ENITITY YOU REPRESENT, IF ANY, DOES
NOT UNCONDITIONALLY AGREE TO THE FOREGOING THEN DO NOT ACCEPT THE LICENSE
AGREEMENTU CONTINUE TO ACCEEPT THE LICENSE AGREEMENT IN ORDER TO DOWNLOAD
THE PRODUCT THEN YOU ARE REPRESENTING AND WARRANTING THAT YOU ARE AUTHORIZED
TO BIND THE ENITITY YOU REPRESENT, IF ANY.
Export Representations and Covenants
You represent, warrant and agree that you
Are not a citizen, national or resident of, and are not under the control
of, the government of any of the following: Cuba, Iran, Sudan, Libya, North
Korea, Syria, nor any other country to which the United States has
prohibited export.
Will not download or otherwise export or re-export the Programs or any
product thereof, directly or indirectly, to the above referenced countries
or to citizens, nationals or residents of those countries.

Are not listed on the United States Department of Treasury lists of
Specially Designated Nationals, Specially Designated Terrorists or Specially
Designated Narcotic Traffickers, nor are you listed on the United States
Department of Commerce Table of Denial Orders.

Will not allow access to or download or otherwise export or re-export the
Programs, directly or indirectly, to any persons on any of the above
referenced lists.

Will not use the Programs for, and will not allow the Programs to be used
for, any purposes prohibited by United States or other law, including,
without limitation, for the development, design, manufacture or production
of nuclear, chemical or biological weapons of mass destruction.

Greenplum License Agreement
 "We," "us," and "our" refers to Greenplum, for and on behalf of itself and
its subsidiaries.  "You" and "your" refers to the individual or entity the
downloading individual represents that wishes to use the Product from
Greenplum.  "Product" refers to the Greenplum software product you wish to
download and use and Product documentation.  "License" refers to your right
to use the Product under the terms of this agreement.
 
We are willing to license the Product to you only upon the condition that
you accept all of the terms contained in this agreement.
 
License Grant
We grant you a nonexclusive, non-transferrable limited license to use the
Product only for the purpose of general evaluation and prototype development
strictly in accordance with our end user documentation, and not for any
other purpose.  If you use the application you develop under this license
for any internal data processing or for any commercial or production
purposes, or you want to use the Product for any purpose other than as
permitted under this agreement, you must contact us, or a Greenplum
reseller, to obtain the appropriate license.  We may audit your use of the
Product. 
 
Ownership / Restrictions
We retain all ownership and intellectual property rights in the Product.
The Product may be installed within a clustered environment and by up to 5
people in the operating environment identified by us.  You may make one copy
of the Product for backup purposes.
 
You are not licensed to, and you agree not to:

- use the Product for your own internal data processing or for any
commercial or production purposes, or use the Product for any purpose except
for general evaluation and prototype development;
- use any application you develop with the Product for any internal data
processing or commercial or production purposes without securing an
appropriate license from us;
- continue to develop your application after you have used it for any
internal data processing, commercial or production purpose without securing
an appropriate license from us;
- remove or modify any Product markings or any notice of our proprietary
rights; 
- make the Product available in any manner to any third party;
- use the Product to provide third party training;
- assign this agreement or give or transfer the Product or an interest in
them to another individual or entity;
- cause or permit reverse engineering (unless required by law for
interoperability), disassembly or decompilation or modification of the
Product; 
- disclose results of any Product benchmark tests without our prior consent;
- use any Greenplum name, trademark or logo; or
- disclose the Product or any information or algorithms or interfaces or
operational aspect relating thereto or embodied therein or to use the
foregoing except as expressly licensed above.

Support
Our technical support organization may at it discretion use reasonable
endeavours to provide technical support, phone support, or updates to you
for the Product licensed under this agreement.
 
Term and Termination
You may terminate this agreement and your license by destroying all copies
of the Product. We have the right to terminate your right to use the Product
if you fail to comply with any of the terms of this agreement, in which case
you shall destroy all copies of the Product.  In any case your license will
terminate 30 days after the download. All your obligations will survive
termination. 

Export
You agree that U.S. export control laws and other applicable export and
import laws govern your use of the Product, including technical data. You
agree that neither the Product nor any direct product thereof will be
exported, directly, or indirectly, in violation of these laws, or will be
used for any purpose prohibited by these laws including, without limitation,
nuclear, chemical, or biological weapons proliferation.
 
Warranty Disclaimer / Liability Limitation
THE PROGRAMS ARE PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. WE FURTHER
DISCLAIM ALL WARRANTIES, EXPRESS AND IMPLIED, INCLUDING WITHOUT LIMITATION,
ANY IMPLIED WARRANTIES OF MERCHANTABILITY, AND FITNESS FOR A PARTICULAR
PURPOSE OR NONINFRINGEMENT.

IN NO EVENT SHALL WE BE LIABLE FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
PUNITIVE OR CONSEQUENTIAL DAMAGES, OR DAMAGES FOR LOSS OF PROFITS, REVENUE,
DATA OR DATA USE, INCURRED BY YOU OR ANY THIRD PARTY, WHETHER IN AN ACTION
IN CONTRACT OR TORT, EVEN IF WE HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGES.  OUR ENTIRE LIABILITY FOR DAMAGES HEREUNDER SHALL IN NO EVENT
EXCEED ONE THOUSAND DOLLARS (U.S. \\\$1,000).

Relationship Between Parties
The relationship between you and us is that of licensee/licensor. Neither
party will represent that it has any authority to assume or create any
obligation, express or implied, on behalf of the other party, nor to
represent the other party as agent, employee, franchisee, or in any other
capacity.   

Open Source Software Restrictions
"Open Source" software - software available without charge for use,
modification or distribution - is often licensed under terms that require
the user to make the user's modifications to the Open Source software or any
software that the user 'combines' with the Open Source software freely
available in source code form.  If you use Open Source software in
conjunction with the Product, you must ensure that your use does not:  (i)
create, or purport to create, obligations of us with respect to the
Greenplum Product; or (ii) grant, or purport to grant, to any third party
any rights to or immunities under our intellectual property or proprietary
rights in the Greenplum Product.  For example, you may not develop a
software Product using a Greenplum Product and an Open Source Product where
such use results in a Product file(s) that contains code from both the
Greenplum Product and the Open Source Product (including without limitation
libraries) if the Open Source Product is licensed under a license that
requires any "modifications" be made freely available.  You also may not
combine the Greenplum Product with programs licensed under the GNU General
Public License ("GPL") in any manner that could cause, or could be
interpreted or asserted to cause, the Greenplum Product or any modifications
thereto to become subject to the terms of the GPL.

General
This agreement is governed by the substantive and procedural laws of
California, excluding choice of laws provision.  You and Greenplum agree to
submit to the exclusive jurisdiction of, and venue in, the courts of San
Francisco, San Mateo, or Santa Clara counties in California in any dispute
arising out of or relating to this agreement, provided that Greenplum may,
in its discretion, institute action in any other jurisdiction in which you
may have a presence. You agree that this agreement is the complete agreement
for the Product and licenses, and this agreement supersedes all prior or
contemporaneous agreements or representations, and may not be amended or
waived except in a mutually agreed signed writing.  If any term of this
agreement is found to be invalid or unenforceable, the remaining provisions
will remain effective.
 
If you have any questions concerning this License Agreement, or desire to
contact Greenplum for any reason, please contact:

Greenplum Corporate Headquarters,
1900 South Norfolk Drive, Suite 224,
San Mateo, CA 94403
 
+1-650-286-8012 (Telephone),
+1-650-286-8010 (Fax)
 
Greenplum may contact you to ask if you had a satisfactory experience
installing and using this software download.

EOF

agreed=
while [ -z "\${agreed}" ] ; do
    cat << EOF

********************************************************************************
        Do you accept the Greenplum Database Performance Monitor 
        license agreement? [yes | no]
********************************************************************************

EOF
    read reply leftover
        case \$reply in
           [yY] | [yY][eE][sS])
                agreed=1
                ;;
           [nN] | [nN][oO])
                cat << EOF

********************************************************************************
      You must accept the license agreement in order to install Greenplum 
      Database Performance Monitor
********************************************************************************
                             
                   **************************************** 
                   *          Exiting installer           *
                   **************************************** 

EOF
                exit 1
                ;;
        esac
done

installPath=
defaultInstallPath=${DEFAULT_INSTALL_PATH}
while [ -z "\${installPath}" ] ; do
    cat << EOF

********************************************************************************
Provide the installation path for Greenplum Database Performance Monitor or 
press ENTER to accept the default installation path: ${DEFAULT_INSTALL_PATH}
********************************************************************************

EOF
    read installPath leftover
        if [ -z "\${installPath}" ] ; then
            installPath=\${defaultInstallPath}
        fi 
        pathVerification=
        while [ -z "\${pathVerification}" ] ; do
            cat << EOF

********************************************************************************
Install Greenplum Database Performance Monitor into <\${installPath}>? [yes | no]
********************************************************************************

EOF
            read pathVerification leftover
                case \$pathVerification in
                    [yY] | [yY][eE][sS])
                        pathVerification=1
                        ;;
                    [nN] | [nN][oO])
                        installPath=
                       ;;
                esac
        done
done

if [ ! -d "\${installPath}" ] ; then
    agreed=
    while [ -z "\${agreed}" ] ; do
    cat << EOF

********************************************************************************
\${installPath} does not exist.
Create \${installPath} ? [ yes | no ]
(Selecting no will exit the installer)
********************************************************************************

EOF
    read reply leftover
        case \$reply in
           [yY] | [yY][eE][sS])
                agreed=1
                ;;
           [nN] | [nN][oO])
                cat << EOF

********************************************************************************
                             Exiting the installer
********************************************************************************

EOF
                exit 1
                ;;
        esac
    done
    mkdir -p \${installPath}
fi

if [ ! -w "\${installPath}" ] ; then
    echo "\${installPath} does not appear to be writeable for your user account."
    echo "Continue?"
    continue=
    while [ -z "\${continue}" ] ; do
        read continue leftover
            case \${continue} in
                [yY] | [yY][eE][sS])
                    continue=1
                    ;;
                [nN] | [nN][oO])
                    echo "Exiting Greenplum Database installation."
                    exit 1
                    ;;
            esac
    done
fi

if [ ! -d \${installPath} ] ; then
    echo "Creating \${installPath}"
    mkdir -p \${installPath}
    if [ \$? -ne "0" ] ; then
        echo "Error creating \${installPath}"
        exit 1
    fi
fi 

echo ""
echo "Extracting product to \${installPath}"
echo ""
EOF_HEADER

#Solaris tail does not recognize -n
if [ "${MPP_ARCH}" = "SOL-x86_64" ] || [ "${MPP_ARCH}" = "SOL10-sparc" ] || [ "${MPP_ARCH}" = "SOL9-sparc" ] || [ "${MPP_ARCH}" = "SOL8-sparc" ] ; then
    cat <<EOF
tail +\${SKIP} \$0 | \${TAR} zxf - -C \${installPath}
EOF
else
    cat <<EOF
tail -n +\${SKIP} \$0 | \${TAR} zxf - -C \${installPath}
EOF
fi
cat << EOF_HEADER
if [ \$? -ne 0 ] ; then
    cat << EOF

********************************************************************************
********************************************************************************
           Error in extracting Greenplum Database Performance Monitor
                               Installation failed
********************************************************************************
********************************************************************************

EOF
    exit 1
fi

sed "s,^GPPERFMONHOME.*,GPPERFMONHOME=\${installPath}," \${installPath}/gpperfmon_path.sh > \${installPath}/gpperfmon_path.sh.tmp
mv \${installPath}/gpperfmon_path.sh.tmp \${installPath}/gpperfmon_path.sh

cat <<EOF

********************************************************************************
Installation complete.
Greenplum Database Performance Monitor is installed in \${installPath}

To complete the environment configuration, please ensure that the
gpperfmon_path.sh file is sourced.

Please read \${installPath}/docs/README to get started.
********************************************************************************
EOF

exit 0

__END_HEADER__
EOF_HEADER
