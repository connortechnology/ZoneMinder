# ==========================================================================
#
# ZoneMinder Sensor Action Module
# Copyright (C) 2019 ZoneMinder LLC
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# ==========================================================================
#
# This module contains the common definitions and functions used by the rest
# of the ZoneMinder scripts
#
package ZoneMinder::Sensor_Action;

use 5.006;
use strict;
use warnings;

require ZoneMinder::Base;
require ZoneMinder::Object;
require ZoneMinder::Monitor;
require ZoneMinder::Logger;

use parent qw(ZoneMinder::Object);

use vars qw/ $table $primary_key %fields /;
$table = 'Sensor_Actions';
$primary_key = 'Id';

%fields = (
        Id        =>  'Id',
        SensorId  =>  'SensorId',
        MonitorId =>  'MonitorId',
        TypeId    =>  'TypeId',
        Action    =>  'Action',
        MinValue  =>  'MinValue',
        MaxValue  =>  'MaxValue',
);

sub Monitor {
	return new ZoneMinder::Monitor($_[0]{MonitorId});
} # end sub Monitor

# Performs the action
sub do {
  my $SA = shift;

  if ( $$SA{Action} =~ /^Preset (\d+)/i ) {
    my $preset_id = $1;

    my $command = join('',
      $ZoneMinder::Config{ZM_PATH_BIN},
      '/zmcontrol.pl --id=',
      $$SA{MonitorId},

      ' --command=presetGoto',
      ' --preset=',
      $preset_id,
    );
    ZoneMinder::Logger::Debug("Command: $command");
    my $output = qx($command);
    chomp($output);
    my $status = $? >> 8;
    if ( $status || ZoneMinder::Logger::logDebugging() ) {
      ZoneMinder::Logger::Debug("Output: $output");
    }
  } # end if Preset

} # end sub do

1;
__END__

=head1 NAME

ZoneMinder::Sensor_Action - Perl Class for Sensor Actions

=head1 SYNOPSIS

use ZoneMinder::Sensor_Action;

=head1 AUTHOR

Isaac Connor, E<lt>isaac@zoneminder.comE<gt>

=head1 COPYRIGHT AND LICENSE

Copyright (C) 2019  ZoneMinder LLC

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself, either Perl version 5.8.3 or,
at your option, any later version of Perl 5 you may have available.


=cut
