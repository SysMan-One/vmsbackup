#define	__MODULE__	"VMSBACKUP"


/*
 *
 *  Title:
 *	Backup
 *
 *  Decription:
 *	Program to read VMS backup tape
 *
 *  Author:
 *	John Douglas CAREY.  (version 2.x, I think)
 *	Sven-Ove Westberg    (version 3.0)
 *      See ChangeLog file for more recent authors.
 *
 *  Net-addess (as of 1986 or so; it is highly unlikely these still work):
 *	john%monu1.oz@seismo.ARPA
 *	luthcad!sow@enea.UUCP
 *
 *
 *  Installation (again, as of 1986):
 *
 *	Computer Centre
 *	Monash University
 *	Wellington Road
 *	Clayton
 *	Victoria	3168
 *	AUSTRALIA
 *
 *  Modification history:
 *
 *	26-JAN-2021 CAW Build on Windows with CMake
 *	29-SEP-2016	RRL	Some changes related structure aligment ...
 *	20-SEP-2016	RRL	Some code cleaning and reformating;
 *				added a logic to recovery broken backup blocks by rescanning Backup Block Header, The,
 *				see scan_bbh () routine to details.
 *
 */

/* MSVC uses _DEBUG */
#ifdef _DEBUG
#define DEBUG 1
#endif

/* Does this system have the magnetic tape ioctls?  The answer is yes for
   most/all unices, and I think it is yes for VMS 7.x with DECC 5.2 (needs
   verification), but it is no for VMS 6.2.  */
#ifndef _WIN32
#ifndef HAVE_MT_IOCTLS
#define HAVE_MT_IOCTLS 1
#endif
#endif

#ifdef HAVE_UNIXIO_H
/* Declarations for read, write, etc.  */
#include	<unixio.h>
#else
#ifdef _WIN32
#include	<direct.h>
#include	<io.h>
#else
#include	<unistd.h>
#include	<sys/file.h>
#endif
#include	<fcntl.h>
#endif

#include	<stdio.h>
#include	<ctype.h>
#include	<errno.h>
#include	<stdlib.h>
#include	<string.h>

#include	<sys/types.h>
#if HAVE_MT_IOCTLS
#include	<sys/ioctl.h>
#include	<sys/mtio.h>
#endif
#ifdef REMOTE
#include	<local/rmt.h>
#include	<sys/stat.h>
#endif

#include	"fabdef.h"

#if !defined(__vax) && !defined(_WIN32)
/* The help claims that mkdir is declared in stdlib.h but it doesn't
   seem to be true.  AXP/VMS 6.2, DECC ?.?.  On the other hand, VAX/VMS 6.2
   seems to declare it in a way which conflicts with this definition.
   This is starting to sound like a bad dream.  */
int mkdir ();
#endif

#include	"vmsbackup.h"
#include	"sysdep.h"

static void debug_dump(const unsigned char* buffer, int dsize, int dtype);

int match ();
char *strlocase ();

/* Byte-swapping routines.  Note that these do not depend on the size
   of datatypes such as short, long, etc., nor do they require us to
   detect the endianness of the machine we are running on.  It is
   possible they should be macros for speed, but I'm not sure it is
   worth bothering.  We don't have signed versions although we could
   add them if needed.  They are, of course little-endian as that is
   the byteorder used by all integers in a BACKUP saveset.
*/

inline unsigned int	__cvt_ul (void *__addr)
{
unsigned char *addr = (unsigned char *) __addr;

	return (((((unsigned long) addr[3] << 8) | addr[2]) << 8)
		| addr[1]) << 8 | addr[0];
}

inline unsigned short	__cvt_uw (void *__addr)
{
unsigned char *addr = (unsigned char *) __addr;

	return (addr[1] << 8) | addr[0];
}


#pragma	pack (push)
#pragma	pack	(1)

typedef struct __bck_blk_hdr {
	unsigned short	w_size,
			w_opsys,
			w_subsys,
			w_applic;

	unsigned 	l_number;

	unsigned char	t_spare_1[20];

	unsigned short	w_struclev,
			w_volnum;

	unsigned	l_crc,
			l_blocksize,
			l_flags;

	unsigned char	t_ssname[32];

	unsigned short	w_fid[3],
			w_did[3];

	unsigned char	t_filename[128],
			b_rtype,
			b_rattrib;

	unsigned short 	w_rsize;

	unsigned char	b_bktsize,
			b_vfcsize;

	unsigned short	w_maxrec;

	unsigned	l_filesize;

	unsigned char	t_spare_2[22];

	unsigned short	w_checksum;
} BCK_BLK_HDR;

typedef struct __bck_rec_hdr {
	unsigned short	w_rsize,
			w_rtype;

	unsigned	l_flags,
			l_address,
			l_spare;
} BCK_REC_HDR;


/* define record types */

#define	BRH_DOL_K_NULL		0
#define	BRH_DOL_K_SUMMARY	1
#define	brh_dol_k_volume	2
#define	brh_dol_k_file		3
#define	brh_dol_k_vbn		4
#define brh_dol_k_physvol	5
#define brh_dol_k_lbn		6
#define	brh_dol_k_fid		7

typedef	struct __itm {
	unsigned short	w_size,
			w_type;

	unsigned char	t_text[1];
} ITM;


#pragma	pack	(pop)



#ifdef	STREAM
char	*def_tapefile = "/dev/rts8";
#else
char	*def_tapefile = "/dev/rmt8";
#endif

unsigned char	filename[128];
int	filesize;
int	afilesize;

unsigned char	recfmt,		/* record format */
		recatt;		/* record attributes */

FILE	*f	= NULL;

/* Number of bytes we have read from the current file so far (or something
   like that; see process_vbn).  */
int	file_count;

unsigned short	reclen, fix, recsize;
int	vfcsize;

/* Number of files we have seen.  */
unsigned int nfiles;

/* Number of blocks in those files.  */
unsigned long nblocks;

int	input_fd;		/* tape file descriptor */

/* Command line stuff.  */

/* The save set that we are listing or extracting.  */
char	*tapefile;

/* We're going to say tflag indicates our best effort at the same
   output format as VMS BACKUP/LIST.  Previous versions of this
   program used tflag for a briefer output format; if we want that
   feature we'll have to figure out what option should control it.  */
int tflag;

int 	cflag, dflag, eflag, sflag, vflag, wflag, xflag, debugflag;

/* Extract files in binary mode.  FIXME: Haven't tried to document
   (here or in the manpage) exactly what that means.  I think it probably
   means (or should mean) to give us raw bits, the same as you would get
   if you read the file in VMS directly using SYS$QIO or some such.  But
   I haven't carefully looked at whether that is what it currently does.  */
int flag_binary;

/* More full listing (/FULL).  */
int flag_full;

/* Which save set are we reading?  */
int	selset;

/* These variables describe the files we will be operating on.  GARGV is
   a vector of GARGC elements, and the elements from GOPTIND to the end
   are the names.  */
char	**gargv;
int 	goptind, gargc;

int	setnr;

#define	LABEL_SIZE	80
char	label[LABEL_SIZE];

char	*block;
/* Default blocksize, as specified in -b option.  */
int	blocksize = 32256;

#if HAVE_MT_IOCTLS
struct	mtop	op;
#endif


FILE *	openfile(unsigned char *fn)
{
unsigned char	ufn[256], ans[80], *p, *q, s, *ext;
int	procf = 1;

	/* copy fn to ufn and convert to lower case */
	for (p = fn, q = ufn; *p; p++, q++)
		*q = isupper(*p) ? *p - 'A' + 'a' : *p;

	*q = '\0';

	/* convert the VMS to UNIX and make the directory path */
	for (p = ufn, q = ++p; *q; q++)
		{
		if (*q == '.' || *q == ']')
			{
			s = *q;
			*q = '\0';

			if (procf && dflag)
#ifndef _WIN32
				mkdir(p, 0777);
#else
				mkdir(p);
#endif

			*q = '/';

			if (s == ']')
				break;
			}
		}

	q++;

	if(!dflag) p = q;

	/* strip off the version number */
	for (; *q && *q != ';'; q++)
		if( *q == '.') ext = q;

	*q = (cflag) ?  ':' : '\0';

	if (procf && wflag)
		{
		printf("extract %s [ny]", filename);
		fflush(stdout);
		fgets(ans, sizeof(ans), stdin);
		if(*ans != 'y') procf = 0;
		}

	/* open the file for writing */
	if (procf)
		return	fopen(p, "wb");

	return	NULL;
}

void	process_summary (
		unsigned char *	bufp,
		size_t		buflen
			)
{
size_t	c;
unsigned char	*text;
unsigned short grp = 0377, usr = 0377, itmcode, itmlen;
unsigned id = 0, blksz = 0, grpsz = 0, bufcnt = 0;
ITM *itm;

	if (!tflag)
		return;

	/* check the header word */
	if (bufp[0] != 1 || bufp[1] != 1)
		{
		printf ("Cannot print summary; invalid data header word 0x%02x%02x\n", bufp[0], bufp[1]);
		return;
		}

	bufp += 2;

	for ( itm = (ITM *) bufp, c = 2; c < buflen; c += itmlen + 4,
			itm = (ITM *) (((char *) itm) + (itmlen + 4)))
		{
		itmlen	= __cvt_uw (&itm->w_size);
		itmcode	= __cvt_uw (&itm->w_type);

		if ( !itmcode )
			break;

		text	= itm->t_text;

		/* Probably should define constants for the cases in this
		   switch, but I don't know anything about whether they
		   have "official" names we should be using or anything
		   like that.  */
		switch (itmcode)
			{
			case 1:	printf ("Save set:          %.*s\n", itmlen, text);
				break;

			case 2:	printf ("Command:           %.*s\n", itmlen, text);
				break;

			case 4:	printf ("Written by:        %.*s\n", itmlen, text);
				break;

			case 5:	if ( itmlen == 4 )
					{
					usr = __cvt_uw (text);
					grp = __cvt_uw (text + 2);
					}

				printf ("UIC:               [%06o, %06o]\n", grp, usr);
				break;

			case 6:	{
				char	dt[32];
				short	dtlen = sprintf(dt, "undefined date");

				if (itmlen != 8 || !(time_vms_to_asc (&dtlen, dt, text) & 1))
					dtlen = sprintf(dt, "error converting date");


				printf ("Date:              %.*s\n", dtlen, dt);

				}
				break;

			case 7:	if (itmlen == 2)
					{
					char *os = "Unknown OS Code";
					unsigned short oscode = __cvt_uw (text);

					switch(oscode)
						{
						case 0x1000:
							os = "OpenVMS IA64";
							break;
						case 0x800:
							os = "OpenVMS AXP";
							break;
						case 0x400:
							os = "OpenVMS VAX";
							break;
						case 0x004:
							os = "RSTS/E";
							break;
						}

					printf ("Operating system: %s (%04x)\n", os, oscode);
					}
				break;

			case 8:	printf ("Operating system version %.*s\n", itmlen, text);
				break;

			case 9:	printf ("Node name:         %.*s\n", itmlen, text);
				break;

			case 10:if (itmlen >= 4)
					id = __cvt_ul (text);
				break;

			case 11:printf ("Written on:        %.*s\n", itmlen, text);
				break;

			case 12:printf ("BACKUP version:    %.*s\n", itmlen, text);
				break;

			case 13:if (itmlen >= 4)
					blksz = __cvt_ul (text);
				break;

			case 14:if (itmlen >= 2)
					grpsz = __cvt_uw (text);
				break;

			case 15:if (itmlen >= 2)
					bufcnt = __cvt_uw (text);
				break;

			default:
				printf("Undefined item code 0x%04x\n", itmcode);
				/* I guess we'll silently ignore these, for future
				   expansion.  */
#ifdef	DEBUG
				debug_dump(text, itmlen, itmcode);
#endif
				break;
			}

		}

	printf ("CPU ID register:   %08x\n", id);
	printf ("Block size:        %u\n", blksz);
	printf ("Group size:        %u\n", grpsz);
	printf ("Buffer count:      %u\n", bufcnt);
	printf ("\n\n");

	/* The extra \n is to provide the blank line between the summary
	   and the list of files that follows.  */
}

void	process_file	(
		unsigned char *	bufp,
		size_t		buflen
			)
{
int	i, c, procf;
short	itmlen, lnch, itmcode, dtlen = 0;
unsigned char	*p, *q, *pdata, *cfname, *sfilename,
	date1[24] = " <None specified>", date2[24] = " <None specified>",
	date3[24] = " <None specified>", date4[24] = " <None specified>";

unsigned nblk, ablk, protection = 0,
/* Number of blocks which should appear in output.  This doesn't
   seem to always be the same as nblk.  */
	blocks, ablocks;

unsigned short reviseno = 0, grp = 0377, usr = 0377, fid[3] = {0,0,0}, extension = 0;
ITM	*itm;


	/* check the header word */
	if (bufp[0] != 1 || bufp[1] != 1)
		{
		printf ("Invalid data header word 0x%02x%02x\n", bufp[0], bufp[1]);
		return;
		}

	bufp += 2;

	for ( itm = (ITM *) bufp, c = 2; c < buflen; c += itmlen + 4,
			itm = (ITM *) (((char *) itm) + (itmlen + 4)))
		{
		itmlen	= __cvt_uw (&itm->w_size);
		itmcode	= __cvt_uw (&itm->w_type);

		pdata	= itm->t_text;

#ifdef DEBUG
 		debug_dump(pdata, itmlen, itmcode);
#endif

		/* Probably should define constants for the cases in this
		   switch, but I don't know anything about whether they
		   have "official" names we should be using or anything
		   like that.  */
		switch (itmcode)
			{
			case 0x2a:
				/* Copy the text into filename, and '\0'-terminate it.  */
				p = pdata;
				q = filename;
				for (i = 0; i < itmlen && q < filename + sizeof (filename) - 1; i++)
					*q++ = *p++;

				*q = '\0';
				break;

			case 0x2b:
				/* In my example, two bytes, 0x1 0x2.  */
				break;

			case 0x2c:
				/* In my example, 6 bytes,
				   0x7a 0x2 0x57 0x0 0x1 0x1.  */
				fid[0] = __cvt_uw(pdata);
				fid[1] = __cvt_uw(pdata + 2);
				fid[2] = __cvt_uw(pdata + 4);
				break;

			case 0x2e:
				/* In my example, 4 bytes, 0x00000004.  Maybe
				   the allocation.  */
				break;

			case 0x2f:
				if (itmlen == 4)
					{
					usr = __cvt_uw (pdata);
					grp = __cvt_uw (pdata + 2);
					}
				break;

			case 0x34:
				recfmt = *pdata;
				recatt = *(pdata + 1);
				recsize = __cvt_uw (pdata + 2);

				/* bytes 4-7 unaccounted for.  */

				ablk = __cvt_uw (pdata + 6);
				nblk = __cvt_uw (pdata + 10)
					/* Adding in the following amount is a
					   change that I brought over from
					   vmsbackup 3.1.  The comment there
					   said "subject to confirmation from
					   backup expert here" but I'll put it
					   in until someone complains.  */
					+ (64 * 1024) * __cvt_uw (pdata + 8);

				lnch = __cvt_uw (pdata + 12);
				/* byte 14 unaccounted for */
				vfcsize = *(pdata + 15);
				if (vfcsize == 0)
					vfcsize = 2;

				/* bytes 16-31 unaccounted for */
				extension = __cvt_uw (pdata + 18);
				break;

			case 0x2d:
				/* In my example, 6 bytes.  hex 2b3c 2000 0000.  */
				break;

			case 0x30:
				/* In my example, 2 bytes.  0x44 0xee.  */
				protection = __cvt_uw (pdata);
				break;

			case 0x31:
				/* In my example, 2 bytes.  hex 0000.  */
				break;
			case 0x32:
				/* In my example, 1 byte.  hex 00.  */
				break;
			case 0x33:
				/* In my example, 4 bytes.  hex 00 0000 00.  */
				break;
			case 0x4b:
				/* In my example, 2 bytes.  Hex 01 00.  */
				break;
			case 0x50:
				/* In my example, 1 byte.  hex 00.  */
				break;
			case 0x57:
				/* In my example, 1 byte.  hex 00.  */
				break;
			case 0x4f:
				/* In my example, 4 bytes.  05 0000 00.  */
				break;
			case 0x35:
				/* In my example, 2 bytes.  04 00.  */
				reviseno = __cvt_uw (pdata);
				break;

			case 0x36:
				/* In my example, 8 bytes.  Presumably a date.  */
				if ( memcmp("\0\0\0\0\0\0\0\0", pdata, 8) && !(time_vms_to_asc (&dtlen, date4, pdata) & 1) )
					strcpy (date4, "error converting date");
				break;
			case 0x37:
				/* In my example, 8 bytes.  Presumably a date.  */
				if ( memcmp("\0\0\0\0\0\0\0\0", pdata, 8) && !(time_vms_to_asc (&dtlen, date1, pdata) & 1) )
					strcpy (date1, "error converting date");
				break;
			case 0x38:
				/* In my example, 8 bytes.  Presumably expires
				   date, since my examples has all zeroes here
				   and BACKUP prints "<None specified>" for
				   expires.  */
				if ( memcmp("\0\0\0\0\0\0\0\0", pdata, 8) && !(time_vms_to_asc (&dtlen, date2, pdata) & 1) )
					strcpy (date2, "error converting date");
				break;
			case 0x39:
				/* In my example, 8 bytes.  Presumably a date.  */
				if ( memcmp("\0\0\0\0\0\0\0\0", pdata, 8) && !(time_vms_to_asc (&dtlen, date3, pdata) & 1) )
					strcpy (date3, "error converting date");
				break;
			case 0x47:
				/* In my example, 4 bytes.  01 00c6 00.  */
				break;
			case 0x48:
				/* In my example, 2 bytes.  88 aa.  */
				break;
			case 0x4a:
				/* In my example, 2 bytes.  01 00.  */
				break;
				/* then comes 0x0, offset 0x2b7.  */
			}
		}

#ifdef	DEBUG
	if (debugflag)
		printf("RMS record's: fmt = %02x, attr = %02x, sz = %d octets, VFC = %d octets\n",
			recfmt, recatt, recsize, vfcsize);
#endif

	/* I believe that "512" here is a fixed constant which should not
	   depend on the device, the saveset, or anything like that.  */
	filesize = (nblk - 1) * 512 + lnch;
	blocks	= (filesize + 511) / 512;
	afilesize = ablk * 512;
	ablocks	= ablk;

#ifdef	DEBUG
	if (debugflag)
		{
		printf("nbk = %d, abk = %d, lnch = %d\n", nblk, ablk, lnch);
		printf("filesize = 0x%x, afilesize = 0x%x\n", filesize, afilesize);
		}
#endif

	/* open the file */
	if ( f )
		{
		fclose(f);
		file_count = reclen = 0;
		}

	procf = 0;

	if (goptind < gargc)
		{
		cfname = dflag ? filename : strrchr(filename, ']') + 1;

		if ( !(sfilename = malloc (strlen (cfname) + 5)) )
			{
			fprintf (stderr, "out of memory\n");
			exit (1);
			}

		if (cflag)
			strcpy(sfilename, cfname);
		else	{
			for (i = 0; i < strlen(cfname) && cfname[i] != ';'; i++)
				sfilename[i] = cfname[i];

			sfilename[i] = '\0';
			}

		for (i = goptind; i < gargc; i++)
			procf |= match (strlocase(sfilename),strlocase(gargv[i]));

		free (sfilename);
		}
	else	procf = 1;


	if ( tflag && procf && !flag_full )
#ifdef HAVE_STARLET
		printf ("%-52s %8d %s\n", filename, blocks, date4);
#else
		printf ("%-52s %8d\n", filename, blocks);
#endif

	if ( tflag && procf && flag_full )
		{
		printf ("%-30.30s File ID:  (%d,%d,%d)\n", filename,fid[0], fid[1], fid[2]);
		printf ("  Size:       %6d/%-6d    Owner:    [%06o,%06o]     Revision:     %6d\n", blocks, ablocks, grp, usr, reviseno);
		printf ("  Protection: (");

		for (i = 0; i <= 3; i++)
			{
			printf("%c:", "SOGW"[i]);
			if (((protection >> (i * 4)) & 1) == 0)
				printf("R");

			if (((protection >> (i * 4)) & 2) == 0)
				printf("W");

			if (((protection >> (i * 4)) & 4) == 0)
				printf("E");

			if (((protection >> (i * 4)) & 8) == 0)
				printf("D");

			if (i != 3)
				printf(",");
			}

		printf(")\n");

#ifdef HAVE_STARLET
		printf("  Created:  %s\n", date4);
		printf("  Revised:  %s (%u)\n", date1, reviseno);
		printf("  Expires:  %s\n", date2);
		printf("  Backup:   %s\n", date3);
#endif

		printf ("  File Organization:  ");
		switch (recfmt & 0xf0)
			{
			case FAB$C_SEQ: printf("Sequential"); break;
			case FAB$C_REL: printf("Relative"); break;
			case FAB$C_IDX: printf("Indexed"); break;
			case FAB$C_HSH: printf("Hashed"); break;
			default: printf("<Unknown 0x%02x>", recfmt &0xf0); break;
			}
		printf("\n");

		printf("  File attributes:    Allocation %u, Extend %d", ablocks, extension);
		printf("\n");

		printf ("  Record format:      ");
		switch (recfmt & 0x0f)
			{
			case FAB$C_UDF: printf ("(UDF/Undefined)"); break;
			case FAB$C_FIX: printf ("Fixed length");
				if (recsize)
					printf (" %u byte records", recsize);
				break;

			case FAB$C_VAR: printf ("Variable length");
				if (recsize)
					printf (", maximum %u bytes", recsize);
				break;

			case FAB$C_VFC: printf ("VFC");
				if (recsize)
					printf (", maximum %u bytes", recsize);
				break;

			case FAB$C_STM:	printf ("Stream"); break;
			case FAB$C_STMLF: printf ("Stream_LF"); break;
			case FAB$C_STMCR: printf ("Stream_CR"); break;
			default: printf ("<Unknown 0x%02x>", recfmt & 0x0f); break;
			}

		printf ("\n");

		printf ("  Record attributes (0x%02x):  ", recatt);
		if (recatt & FAB$M_FTN) printf ("Fortran ");
		if (recatt & FAB$M_PRN) printf ("Print file ");
		if (recatt & FAB$M_CR) printf ("Carriage return carriage control ");
		if (recatt & FAB$M_BLK) printf ("Non-spanned");

		printf ("\n");
		}

	if ( xflag && procf)
		{
		/* open file */
		if ( (f = openfile(filename)) && vflag)
			printf("extracting %s\n", filename);
		}

	++nfiles;
	nblocks += blocks;
}

/*
 *
 *  process a virtual block record (file record)
 *
 */
void	process_vbn	(
		unsigned char *	buffer,
		size_t		rsize
		)
{
int	c, i, j;

	if ( !f )
		return;

	for (i = 0; file_count + i < filesize && i < rsize; )
		{
		switch (recfmt)
			{
			case FAB$C_FIX:
				reclen = reclen ? reclen : recsize;

				fputc(buffer[i], f);
				i++;
				reclen--;
				break;

			case FAB$C_VAR:
			case FAB$C_VFC:
				if (reclen == 0)
					{
					fix = reclen = __cvt_uw (&buffer[i]);
					if (flag_binary)
						for (j = 0; j < 2; j++)
							fputc (buffer[i+j], f);

					i += 2;
					if (recfmt == FAB$C_VFC)
						{
						if (flag_binary)
							for (j = 0; j < vfcsize; j++)
								fputc (buffer[i+j], f);

						i += vfcsize;
						reclen -= vfcsize;
						}
					}
				else if (reclen == fix && recatt == FAB$M_FTN)
					{
					/****
					if (buffer[i] == '0')
						fputc('\n', f);
					else if (buffer[i] == '1')
						fputc('\f', f);
					*** sow ***/
					fputc(buffer[i], f); /** sow **/
					i++;
					reclen--;
					}
				else	{
					fputc(buffer[i], f);
					i++;
					reclen--;
					}



				if ( !reclen )
					{
					if (!flag_binary)
						fputc('\n', f);

					if (i & 1)
						{
						if (flag_binary)
							fputc (buffer[i], f);

						i++;
						}
					}
				break;

			case FAB$C_STM:
			case FAB$C_STMLF:
				if (reclen < 0)
					printf("STREAM\n");

				if ( !reclen == 0)
					reclen = 512;

				c = buffer[i++];
				reclen--;

				if (c == '\n')
					reclen = 0;

				fputc(c, f);
				break;

			case FAB$C_STMCR:
				c = buffer[i++];

				if (c == '\r' && !flag_binary)
					fputc('\n', f);
				else	fputc(c, f);

				break;

			default:
				fclose(f); f = NULL;
				remove(filename);
				fprintf(stderr, "Invalid record format =0x%02x/%d\n", recfmt, recfmt);
				return;
			}
		}

	file_count += i;
}


#define	BBH$K_SZ	256

void	scan_bbh	(
		char	*bufp

		)
{
int	status;
unsigned short	bhsize;
unsigned	bsize, i = 0;
BCK_BLK_HDR *	bbh = (BCK_BLK_HDR *) bufp;

	printf("[0x%08X] Start scanning for Backup Block Header ...\n",
		lseek (input_fd, 0, SEEK_CUR) - blocksize);

	do	{
		/*
		 * Reading by 512 bytes
		 */
		if ( BBH$K_SZ != (status = read(input_fd, block, BBH$K_SZ)) )
			{
			fprintf(stderr, "Error reading %d, got %d (expected %d), errno = %d", input_fd, 512, status, errno);
			exit(1);
			}

		bhsize	= __cvt_uw (&bbh->w_size);
		bsize	= __cvt_ul (&bbh->l_blocksize);

#ifdef	DEBUG
		if (debugflag)
			fprintf(stderr, "[0x%08X] Backup block: header length = %d, size = %d, type (DATA=1/XOR=2) = %5d\n",
				lseek (input_fd, 0, SEEK_CUR) - BBH$K_SZ, bhsize, bsize, bbh->w_applic);
#endif

		status = 0;

		if ( BBH$K_SZ != bhsize )
			continue;

		if ( blocksize != bsize )
			continue;

		if ( bbh->w_applic != 1 )
			continue;

		if ( bbh->t_filename[0] > (sizeof(bbh->t_filename) - 1) )
			continue;

		if ( bbh->t_ssname[0] > (sizeof(bbh->t_ssname) - 1) )
			continue;

		status = 1;

		} while ( !status );


	/* Set file position to begin of the Backup Block Header */
	lseek (input_fd, -BBH$K_SZ, SEEK_CUR);
}

/*
 *
 *  process a backup block
 *
 */
void	process_block	(
		char	*bufp,
		int	buflen
			)
{
unsigned short	bhsize, rsize, rtype;
unsigned	bsize, i = 0;
BCK_BLK_HDR *	bbh;
BCK_REC_HDR *	brh;


	/* read the backup block header */
	bbh	= (BCK_BLK_HDR *) bufp;
	bhsize	= __cvt_uw (&bbh->w_size);
	bsize	= __cvt_ul (&bbh->l_blocksize);

	/* check the validity of the header block */
	if ( bhsize != sizeof(BCK_BLK_HDR) )
		{
		fprintf (stderr, "[0x%08X] Invalid header block size: expected %d got 0x%x/%d\n",
			lseek(input_fd, 0, SEEK_CUR) - blocksize, sizeof (BCK_BLK_HDR), bhsize, bhsize);

		scan_bbh (bufp);

		return;
		}

	if ( bsize && bsize != buflen)
		{
		fprintf(stderr, "[0x%08X] Invalid block size got %d, expected 0x%x/%d\n",
			lseek(input_fd, 0, SEEK_CUR) - blocksize, bsize, buflen, buflen);

		scan_bbh (bufp);

		return;
		}

	if ( bbh->w_applic == 2 )
		return;


#ifdef	DEBUG
	if (debugflag)
		printf("[0x%08X] Backup block: header length = %d, size = %d, type (DATA=1/XOR=2) = %5d, csum = %x04\n",
			lseek (input_fd, 0, SEEK_CUR), bhsize, bsize, bbh->w_applic, bbh->w_checksum);
#endif


	bufp += (i = sizeof(BCK_BLK_HDR));

	/* read the records */
	for (brh = (BCK_REC_HDR *) (bufp) ; i < bsize; i += rsize, bufp += rsize )
		{
		/* read the backup record header */
		brh = (BCK_REC_HDR *) bufp;

		rtype = __cvt_uw (&brh->w_rtype);
		rsize = __cvt_uw (&brh->w_rsize);

#ifdef	DEBUG
		if (debugflag)
			printf("+%06d: Record: type = 0x%x, size = %-5d, flags = 0x%x, addr = 0x%08x\n",
				i, rtype, rsize, brh->l_flags, brh->l_address);
#endif

		bufp += sizeof(BCK_REC_HDR);
		i += sizeof(BCK_REC_HDR);

		switch (rtype)
			{


			case BRH_DOL_K_SUMMARY:
#ifdef	DEBUG
				if (debugflag)
					printf("rtype = Save Set summary\n");
#endif

				process_summary (bufp, rsize);
				break;

			case brh_dol_k_file:
#ifdef	DEBUG
				if (debugflag)
					printf("rtype = FILE\n");
#endif


				process_file(bufp, rsize);
				break;

			case brh_dol_k_vbn:
#ifdef	DEBUG
				if (debugflag)
					printf("rtype = VBN\n");
#endif

				process_vbn(bufp, rsize);
				break;


#ifdef	DEBUG


			case BRH_DOL_K_NULL:
			if (debugflag)
				printf("rtype = null\n");

			/* This is the type used to pad to the end of
			   a block.  */
			break;

			case brh_dol_k_physvol:
				if (debugflag)
					printf("rtype = PHYSVOL\n");
				break;

			case brh_dol_k_lbn:
				if (debugflag)
					printf("rtype = LBN\n");
				break;

			case brh_dol_k_fid:
				if (debugflag)
					printf("rtype = FID\n");
				break;

			case brh_dol_k_volume:
				if (debugflag)
					printf("rtype = VOLUME\n");
				break;

			default:
				/* It is quite possible that we should just skip
				this without even printing a warning.  */
				if (debugflag)
					{
					fprintf (stderr," Warning: unrecognized record type\n");
					fprintf (stderr, " record type = %d, size = %d\n", rtype, rsize);
					}
				break;
#endif
			}
		}
}



int	rdhead	(void)
{
int i, nfound;
char name[80];

	nfound = 1;

	/* read the tape label - 4 records of 80 bytes */
	while ( (i = read(input_fd, label, LABEL_SIZE)) )
		{
		if ( i != LABEL_SIZE)
			{
			fprintf(stderr, "Snark: bad label record\n");
			exit(1);
			}

		if ( !strncmp(label, "VOL1", 4) )
			{
			sscanf(label + 4, "%14s", name);

			if(vflag || tflag)
				printf("Volume: %s\n",name);
			}

			if ( !strncmp(label, "HDR1",4) )
				{
				sscanf(label+4, "%14s", name);
				sscanf(label+31, "%4d", &setnr);
				}

		/* get the block size */
		if ( !strncmp(label, "HDR2", 4) )
			{
			nfound = 0;
			sscanf(label+5, "%5d", &blocksize);
#ifdef	DEBUG
			if (debugflag)
				printf("blocksize = %d\n", blocksize);
#endif
			}
		}

	if( (vflag || tflag) && !nfound )
		printf("Saveset name: %s   number: %d\n", name, setnr);

	/* get the block buffer */
	if ( !(block = (char *) malloc(blocksize)) )
		{
		fprintf(stderr, "memory allocation for block failed\n");
		exit(1);
		}

	return	nfound;
}

void	rdtail	(void)
{
int i;
char name[80];

	/* read the tape label - 4 records of 80 bytes */
	while ( (i = read(input_fd, label, LABEL_SIZE)) )
		{
		if (i != LABEL_SIZE)
			{
			fprintf(stderr, "Snark: bad label record\n");
			exit(1);
			}

		if ( !strncmp(label, "EOF1", 4) )
			{
			sscanf(label + 4, "%14s", name);

			if ( vflag || tflag )
				printf("End of saveset: %s\n\n\n",name);
			}
		}
}


/* Perform the actual operation.  The way this works is that main () parses
   the arguments, sets up the global variables like cflags, and calls us.
   Does not return--it always calls exit ().  */
void	vmsbackup	(void)
{
int	i, eoffl;

/* Nonzero if we are reading from a saveset on disk (as
   created by the /SAVE_SET qualifier to BACKUP) rather than from
   a tape.  */
int ondisk;

/* Read-only plus binary if on Windows */
int openflag = O_RDONLY;

#ifdef _WIN32
    openflag |= O_BINARY;
#endif

	if (tapefile == NULL)
		tapefile = def_tapefile;

	/* open the tape file */
	if ( 0 > (input_fd = open(tapefile, openflag)) )
		{
		perror(tapefile);
		exit(1);
		}

#if HAVE_MT_IOCTLS
	/* rewind the tape */
	op.mt_op = MTREW;
	op.mt_count = 1;

	if ( 0 > (i = ioctl(input_fd, MTIOCTOP, &op)) )
		{
		if (errno == EINVAL || errno == ENOTTY)
			ondisk = 1;
		else	{
			perror(tapefile);
			exit(1);
			}
		}
#else
	ondisk = 1;
#endif

	if (ondisk)
		{
		/* process_block wants this to match the size which
		   backup writes into the header.  Should it care in
		   the ondisk case?  */
#ifdef FIXME
		/* This is already initialized in the variable definition,
		   and having this here makes the '-b' option a do-nothing
		   sort of thing, which makes it impossible to read
		   RSTS/E save sets */
		blocksize = 32256;
#endif
		if ( !(block = malloc (blocksize)) )
			{
			fprintf(stderr, "memory allocation for block failed\n");
			exit(1);
			}

		eoffl = 0;
		}
	else	eoffl = rdhead();

	nfiles = nblocks = 0;

	/* read the backup tape blocks until end of tape */
	while ( !eoffl )
		{
		if(sflag && setnr != selset)
			{
			if (ondisk)
				{
				fprintf(stderr, "-s not supported for disk savesets\n");
				exit(1);
				}
#if HAVE_MT_IOCTLS
			op.mt_op = MTFSF;
			op.mt_count = 1;

			if ( 0 > (i = ioctl(input_fd, MTIOCTOP, &op)) )
				{
				perror(tapefile);
				exit(1);
				}
#else
			abort ();
#endif
			i = 0;
			}
		else	i = read(input_fd, block, blocksize);

		if ( !i )
			{
			if (ondisk)
				{
				/* No need to support multiple save sets.  */
				eoffl = 1;
				}
			else	{
				if ( vflag || tflag )
					printf ("\nTotal of %u files, %lu blocks\n", nfiles, nblocks);

				rdtail();
				eoffl = rdhead();
				}
			}
		else if (i == -1)
			{
			perror ("error reading saveset");
			exit (1);
			}
		else if (i != blocksize)
			{
			fprintf(stderr, "bad block read i = %d\n", i);
			exit(1);
			}
		else	{
			eoffl = 0;
			process_block(block, blocksize);
			}
		}


	if ( vflag || tflag )
		{
		if (ondisk)
			printf ("\nTotal of %u files, %u blocks\nEnd of save set\n", nfiles, nblocks);
		else	printf("End of tape\n");
		}

	/* close the tape */
	close(input_fd);

#ifdef	NEWD
	/* close debug file */
	fclose(lf);
#endif

	/* exit cleanly */
	exit(0);
}


static void debug_dump(const unsigned char* buffer, int itmlen, int itmcode)
{

	if (!debugflag)
		return;


	/* reduce a maximum output size */
	itmlen = itmlen > 128 ? 128 : itmlen;

	printf("dsize = 0x%x/%d octets, dtype = 0x%x/%d, hex dump follows -----------\n ", itmlen, itmlen, itmcode, itmcode);

	for (; itmlen; itmlen--, buffer++)
		printf("%02x ", *buffer);

	printf("\n");

}
