#!/usr/bin/perl
# -*- Mode: perl; indent-tabs-mode: nil; cperl-indent-level: 4 -*-

use strict;
use diagnostics;
use IO::File;
use Time::Local;

my $pristine = 1;
my $kernver;

sub get_tag()
{
    my $tag;

    my $tagfile = new IO::File;
    if (!$tagfile->open("CVS/Tag")) {
        return "HEAD";
    } else {
        my $tmp = <$tagfile>;
        $tagfile->close();

        $tmp =~ m/T(.*)/;
        return $1;
    }
}

sub get_latest_mtime()
{
    my %months=("Jan" => 0, "Feb" => 1, "Mar" => 2, "Apr" => 3, "May" => 4,
                "Jun" => 5, "Jul" => 6, "Aug" => 7, "Sep" => 8, "Oct" => 9,
                "Nov" => 10, "Dec" => 11);

    my $last_mtime = 0;
    my @entries = `find . -name Entries`;
    my $entry_file;
    foreach $entry_file (@entries) {
        chomp($entry_file);
        my $entry = new IO::File;
        if (!$entry->open($entry_file)) {
            die "unable to open $entry_file: $!\n";
        }
        my $line;
        while (defined($line = <$entry>)) {
            chomp($line);
            #print "line: $line\n";
            my ($junk, $file, $version, $date) = split(/\//, $line);

            #print "junk: $junk\nfile: $file\nver: $version\ndate: $date\n";
            #print "last_mtime: " . localtime($last_mtime) . "\n";

            if ($junk eq "D" ||
                $file eq "lustre.spec.in" ||
                $file !~ m/\.(c|h|am|in)$/) {
                next;
            }

            my $cur_dir = $entry_file;
            $cur_dir =~ s/\/CVS\/Entries$//;
            my @statbuf = stat("$cur_dir/$file");
            my $mtime = $statbuf[9];
            my $local_date = gmtime($mtime);
            if ($local_date ne $date &&
                $file ne "lustre.spec.in") {
                #print "$file : " . localtime($mtime) . "\n";
                $pristine = 0;
            }

            if ($mtime > $last_mtime) {
                $last_mtime = $mtime;
            }

            if ($date) {
                my @t = split(/ +/, $date);
                if (int(@t) != 5) {
                    #print "skipping: $date\n";
                    next;
                }
                my ($hours, $min, $sec) = split(/:/, $t[3]);
                my ($mon, $mday, $year) = ($t[1], $t[2], $t[4]);
                my $secs = 0;
                $mon = $months{$mon};
                $secs = timelocal($sec, $min, $hours, $mday, $mon, $year);
                if ($secs > $last_mtime) {
                    $last_mtime = $secs;
                }
            }
        }
        $entry->close();
    }
    return $last_mtime;
}

sub get_linuxdir()
{
    my $config = new IO::File;
    my ($line, $dir);
    if (!$config->open("Makefile")) {
        die "Run ./configure first\n";
    }
    while (defined($line = <$config>)) {
        chomp($line);
        if ($line =~ /LINUX = (.*)/) {
            $dir = $1;
            last;
        }
    }
    $config->close();
    my $ver = new IO::File;
    if (!$ver->open("$dir/include/linux/version.h")) {
        die "Run make dep on $dir\n";
    }
    while(defined($line = <$ver>)) {
        $line =~ /\#define UTS_RELEASE "(.*)"/;
        if ($1) {
            $kernver = $1;
            last;
        }
    }
    $ver->close();
    chomp($kernver);
    $dir =~ s/\//\./g;
    return $dir;
}

sub generate_ver($$$)
{
    my $tag = shift;
    my $mtime = shift;
    my $linuxdir = shift;

    #print "localtime: " . localtime($mtime) . "\n";

    my ($sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $isdst) =
      localtime($mtime);
    $year += 1900;
    $mon++;
    my $show_last = sprintf("%04d%02d%02d%02d%02d%02d", $year, $mon, $mday,
                            $hour, $min, $sec);

    print "#define BUILD_VERSION \"";
    if ($pristine) {
        print "$tag-$show_last-PRISTINE-$linuxdir-$kernver\"\n";
    } else {
        print "$tag-$show_last-CHANGED-$linuxdir-$kernver\"\n";
    }
}

if ($ARGV[0]) {
    chdir($ARGV[0]);
}
my $linuxdir = get_linuxdir();
my $tag = get_tag();
my $mtime = get_latest_mtime();
generate_ver($tag, $mtime, $linuxdir);

exit(0);
