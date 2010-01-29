#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <getopt.h>

void usage(const char *exec)
{
	fprintf(stderr,
			"%s: \n"
			"    -t <type> -a <active> -p <primary host> -d <primary device> -m <primary mountpoint>\n"
			"    -q <mirror host> -e <mirror device> -n <mirror mountpoint>\n", exec);
	exit(1);
}

int
main(int argc, char **argv)
{
	int			opt, bad_opts=0;
	char		type=0;
	char		active=0;
	char		*p_host=NULL, *m_host=NULL;
	char		*p_dev=NULL, *m_dev=NULL;
	char		*p_mp=NULL, *m_mp=NULL;

	while ((opt = getopt(argc, argv, "ht:a:p:d:m:q:e:n:")) != -1)
	{
		switch(opt)
		{
			case 't':
				/* type 'n' or 'e' */
				if (optarg == NULL || strlen(optarg) > 1)
				{
					fprintf(stderr, "Invalid type\n");
					usage(argv[0]);
				}

				type = *optarg;
				if (type == 'n')
					fprintf(stdout, "type is NFS\n");
				else if (type == 'e')
					fprintf(stdout, "type is EMC\n");
				else
				{
					fprintf(stderr, "Invalid type\n");
					usage(argv[0]);
				}

				break;
			case 'a':
				/* active indicator 'p' or 'm' */
				if (optarg == NULL || strlen(optarg) > 1)
				{
					fprintf(stderr, "Invalid active\n");
					usage(argv[0]);
				}

				active = *optarg;
				if (active == 'p')
					fprintf(stdout, "active is primary\n");
				else if (active == 'm')
					fprintf(stdout, "active is mirror\n");
				else
				{
					fprintf(stderr, "Invalid active\n");
					usage(argv[0]);
				}
				break;
			case 'p':
				/* primary hostname */
				p_host = optarg;
				break;
			case 'd':
				/* primary device-id */
				p_dev = optarg;
				break;
			case 'm':
				/* primary mount-point */
				p_mp = optarg;
				break;
			case 'q':
				/* mirror host */
				m_host = optarg;
				break;
			case 'e':
				/* mirror device-id */
				m_dev = optarg;
				break;
			case 'n':
				/* mirror mount-point */
				m_mp = optarg;
				break;
			case 'h':
			default:
				usage(argv[0]);
				/* not reached */
				break;
		}
	}

	if (type == 0)
	{
		fprintf(stderr, "Bad type\n");
		bad_opts++;
	}

	if (active == 0)
	{
		fprintf(stderr, "Bad active\n");
		bad_opts++;
	}

	if (p_host == NULL || m_host == NULL)
	{
		fprintf(stderr, "Bad host\n");
		bad_opts++;
	}

	if (p_dev == NULL || m_dev == NULL)
	{
		fprintf(stderr, "Bad device\n");
		bad_opts++;
	}

	if (p_mp == NULL || m_mp == NULL)
	{
		fprintf(stderr, "Bad mountpoint\n");
		bad_opts++;
	}

	if (bad_opts != 0)
	{
		usage(argv[0]);
		/* not reached */
	}

	fprintf(stdout, "type %c active %c primary %s:'%s':'%s' mirror %s:'%s':'%s'\n",
			type, active, p_host, p_dev, p_mp, m_host, m_dev, m_mp);

	if (type == 'n')
	{
		char cmd[1000];
		char *target_host, *target_dev, *target_mountpoint;
		int status;

		if (active == 'p')
		{
			target_host = m_host;
			target_dev = m_dev;
			target_mountpoint = m_mp;
		}
		else
		{
			target_host = p_host;
			target_dev = p_dev;
			target_mountpoint = p_mp;
		}

		snprintf(cmd, sizeof(cmd), "ssh %s /bin/mount %s %s", target_host, target_dev, target_mountpoint);
		status = system(cmd);		

		if (status == 0)
			return 0;
		else
			return status;
	}

	exit(0);
}
