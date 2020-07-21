#include "flexnet/util/mime_type.hpp" // IWYU pragma: associated

namespace flexnet {

MimeType::MimeType()
    : mime_types_()
    , default_mime_type_("application/octet-stream")
{
  DETACH_FROM_SEQUENCE(sequence_checker_);

  mime_types_["."] = "application/x-";
  mime_types_[".001"] = "application/x-001";
  mime_types_[".301"] = "application/x-301";
  mime_types_[".323"] = "text/h323";
  mime_types_[".3gp"] = "video/3gpp";
  mime_types_[".906"] = "application/x-906";
  mime_types_[".907"] = "drawing/907";
  mime_types_[".ivf"] = "video/x-ivf";
  mime_types_[".a11"] = "application/x-a11";
  mime_types_[".acp"] = "audio/x-mei-aac";
  mime_types_[".ai"] = "application/postscript";
  mime_types_[".aif"] = "audio/aiff";
  mime_types_[".aifc"] = "audio/aiff";
  mime_types_[".aiff"] = "audio/aiff";
  mime_types_[".anv"] = "application/x-anv";
  mime_types_[".asa"] = "text/asa";
  mime_types_[".asf"] = "video/x-ms-asf";
  mime_types_[".asp"] = "text/asp";
  mime_types_[".asx"] = "video/x-ms-asf";
  mime_types_[".au"] = "audio/basic";
  mime_types_[".avi"] = "video/x-msvideo";
  mime_types_[".awf"] = "application/vnd.adobe.workflow";
  mime_types_[".biz"] = "text/xml";
  mime_types_[".bmp"] = "application/x-MS-bmp";
  mime_types_[".bot"] = "application/x-bot";
  mime_types_[".c4t"] = "application/x-c4t";
  mime_types_[".c90"] = "application/x-c90";
  mime_types_[".cab"] = "application/x-shockwave-flash";
  mime_types_[".cal"] = "application/x-cals";
  mime_types_[".cat"] = "application/vnd.ms-pki.seccat";
  mime_types_[".cdf"] = "application/x-netcdf";
  mime_types_[".cdr"] = "application/x-cdr";
  mime_types_[".cel"] = "application/x-cel";
  mime_types_[".cer"] = "application/x-x509-ca-cert";
  mime_types_[".cg4"] = "application/x-g4";
  mime_types_[".cgm"] = "application/x-cgm";
  mime_types_[".chm"] = "application/mshelp";
  mime_types_[".cit"] = "application/x-cit";
  mime_types_[".class"] = "java/*";
  mime_types_[".cml"] = "text/xml";
  mime_types_[".cmp"] = "application/x-cmp";
  mime_types_[".cmx"] = "application/x-cmx";
  mime_types_[".cot"] = "application/x-cot";
  mime_types_[".crl"] = "application/pkix-crl";
  mime_types_[".crt"] = "application/x-x509-ca-cert";
  mime_types_[".csi"] = "application/x-csi";
  mime_types_[".css"] = "text/css";
  mime_types_[".cut"] = "application/x-cut";
  mime_types_[".dbf"] = "application/x-dbf";
  mime_types_[".dbm"] = "application/x-dbm";
  mime_types_[".dbx"] = "application/x-dbx";
  mime_types_[".dcd"] = "text/xml";
  mime_types_[".dcx"] = "application/x-dcx";
  mime_types_[".der"] = "application/x-x509-ca-cert";
  mime_types_[".dgn"] = "application/x-dgn";
  mime_types_[".dib"] = "application/x-dib";
  mime_types_[".dll"] = "application/x-msdownload";
  mime_types_[".doc"] = "application/msword";
  mime_types_[".dot"] = "application/msword";
  mime_types_[".drw"] = "application/x-drw";
  mime_types_[".dtd"] = "text/xml";
  mime_types_[".dwf"] = "application/x-dwf";
  mime_types_[".dwg"] = "application/x-dwg";
  mime_types_[".dxb"] = "application/x-dxb";
  mime_types_[".dxf"] = "application/x-dxf";
  mime_types_[".edn"] = "application/vnd.adobe.edn";
  mime_types_[".emf"] = "application/x-emf";
  mime_types_[".eml"] = "message/rfc822";
  mime_types_[".ent"] = "text/xml";
  mime_types_[".epi"] = "application/x-epi";
  mime_types_[".eps"] = "application/postscript";
  mime_types_[".etd"] = "application/x-ebx";
  mime_types_[".exe"] = "application/x-msdownload";
  mime_types_[".fax"] = "image/fax";
  mime_types_[".fdf"] = "application/vnd.fdf";
  mime_types_[".fif"] = "application/fractals";
  mime_types_[".fo"] = "text/xml";
  mime_types_[".frm"] = "application/x-frm";
  mime_types_[".g4"] = "application/x-g4";
  mime_types_[".gbr"] = "application/x-gbr";
  mime_types_[".gif"] = "image/gif";
  mime_types_[".gl2"] = "application/x-gl2";
  mime_types_[".gp4"] = "application/x-gp4";
  mime_types_[".gz"] = "application/x-gzip";
  mime_types_[".hgl"] = "application/x-hgl";
  mime_types_[".hlp"] = "application/mshelp";
  mime_types_[".hmr"] = "application/x-hmr";
  mime_types_[".hpg"] = "application/x-hpgl";
  mime_types_[".hpl"] = "application/x-hpl";
  mime_types_[".hqx"] = "application/mac-binhex40";
  mime_types_[".hrf"] = "application/x-hrf";
  mime_types_[".hta"] = "application/hta";
  mime_types_[".htc"] = "text/x-component";
  mime_types_[".htm"] = "text/html";
  mime_types_[".html"] = "text/html";
  mime_types_[".hts"] = "text/html";
  mime_types_[".htt"] = "text/webviewhtml";
  mime_types_[".htx"] = "text/html";
  mime_types_[".icb"] = "application/x-icb";
  mime_types_[".ico"] = "application/x-ico";
  mime_types_[".iff"] = "application/x-iff";
  mime_types_[".ig4"] = "application/x-g4";
  mime_types_[".igs"] = "application/x-igs";
  mime_types_[".iii"] = "application/x-iphone";
  mime_types_[".img"] = "application/x-img";
  mime_types_[".ins"] = "application/x-internet-signup";
  mime_types_[".isp"] = "application/x-internet-signup";
  mime_types_[".jar"] = "application/java-archive";
  mime_types_[".java"] = "java/*";
  mime_types_[".jfif"] = "image/jpeg";
  mime_types_[".jpe"] = "image/jpeg";
  mime_types_[".jpeg"] = "image/jpeg";
  mime_types_[".jpg"] = "image/jpeg";
  mime_types_[".jpz"] = "image/jpeg";
  mime_types_[".js"] = "text/javascript";
  mime_types_[".json"] = "application/json";
  mime_types_[".jsp"] = "text/html";
  mime_types_[".la1"] = "audio/x-liquid-file";
  mime_types_[".lar"] = "application/x-laplayer-reg";
  mime_types_[".latex"] = "application/x-latex";
  mime_types_[".lavs"] = "audio/x-liquid-secure";
  mime_types_[".lbm"] = "application/x-lbm";
  mime_types_[".lmsff"] = "audio/x-la-lms";
  mime_types_[".log"] = "text/plain";
  mime_types_[".ls"] = "application/x-javascript";
  mime_types_[".ltr"] = "application/x-ltr";
  mime_types_[".m1v"] = "video/x-mpeg";
  mime_types_[".m2v"] = "video/x-mpeg";
  mime_types_[".m3u"] = "audio/mpegurl";
  mime_types_[".m4e"] = "video/mpeg4";
  mime_types_[".mac"] = "application/x-mac";
  mime_types_[".man"] = "application/x-troff-man";
  mime_types_[".math"] = "text/xml";
  mime_types_[".mdb"] = "application/x-mdb";
  mime_types_[".mfp"] = "application/x-shockwave-flash";
  mime_types_[".mht"] = "message/rfc822";
  mime_types_[".mhtml"] = "message/rfc822";
  mime_types_[".mi"] = "application/x-mi";
  mime_types_[".mid"] = "audio/midi";
  mime_types_[".midi"] = "audio/midi";
  mime_types_[".mil"] = "application/x-mil";
  mime_types_[".mml"] = "text/xml";
  mime_types_[".mnd"] = "audio/x-musicnet-download";
  mime_types_[".mns"] = "audio/x-musicnet-stream";
  mime_types_[".mocha"] = "application/x-javascript";
  mime_types_[".mov"] = "video/quicktime";
  mime_types_[".movie"] = "video/x-sgi-movie";
  mime_types_[".mp1"] = "audio/mp1";
  mime_types_[".mp2"] = "audio/x-mpeg";
  mime_types_[".mp2v"] = "video/mpeg";
  mime_types_[".mp3"] = "audio/x-mpeg";
  mime_types_[".mp4"] = "video/mp4";
  mime_types_[".mpa"] = "video/x-mpg";
  mime_types_[".mpd"] = "application/vnd.ms-project";
  mime_types_[".mpe"] = "video/mpeg";
  mime_types_[".mpeg"] = "video/mpeg";
  mime_types_[".mpg"] = "video/mpeg";
  mime_types_[".mpg4"] = "video/mp4";
  mime_types_[".mpga"] = "video/mpeg";
  mime_types_[".mpp"] = "application/vnd.ms-project";
  mime_types_[".mps"] = "video/x-mpeg";
  mime_types_[".mpt"] = "application/vnd.ms-project";
  mime_types_[".mpv"] = "video/mpg";
  mime_types_[".mpv2"] = "video/mpeg";
  mime_types_[".mpw"] = "application/vnd.ms-project";
  mime_types_[".mpx"] = "application/vnd.ms-project";
  mime_types_[".mtx"] = "text/xml";
  mime_types_[".mxp"] = "application/x-mmxp";
  mime_types_[".net"] = "image/pnetvue";
  mime_types_[".nrf"] = "application/x-nrf";
  mime_types_[".nws"] = "message/rfc822";
  mime_types_[".odc"] = "text/x-ms-odc";
  mime_types_[".out"] = "application/x-out";
  mime_types_[".p10"] = "application/pkcs10";
  mime_types_[".p12"] = "application/x-pkcs12";
  mime_types_[".p7b"] = "application/x-pkcs7-certificates";
  mime_types_[".p7c"] = "application/pkcs7-mime";
  mime_types_[".p7m"] = "application/pkcs7-mime";
  mime_types_[".p7r"] = "application/x-pkcs7-certreqresp";
  mime_types_[".p7s"] = "application/pkcs7-signature";
  mime_types_[".pc5"] = "application/x-pc5";
  mime_types_[".pci"] = "application/x-pci";
  mime_types_[".pcl"] = "application/x-pcl";
  mime_types_[".pcx"] = "application/x-pcx";
  mime_types_[".pdf"] = "application/pdf";
  mime_types_[".pdx"] = "application/vnd.adobe.pdx";
  mime_types_[".pfx"] = "application/x-pkcs12";
  mime_types_[".pgl"] = "application/x-pgl";
  mime_types_[".pic"] = "application/x-pic";
  mime_types_[".pko"] = "application/vnd.ms-pki.pko";
  mime_types_[".pl"] = "application/x-perl";
  mime_types_[".plg"] = "text/html";
  mime_types_[".pls"] = "audio/scpls";
  mime_types_[".plt"] = "application/x-plt";
  mime_types_[".png"] = "image/png";
  mime_types_[".pot"] = "application/mspowerpoint";
  mime_types_[".ppa"] = "application/vnd.ms-powerpoint";
  mime_types_[".ppm"] = "application/x-ppm";
  mime_types_[".pps"] = "application/mspowerpoint";
  mime_types_[".ppt"] = "application/mspowerpoint";
  mime_types_[".ppz"] = "application/mspowerpoint";
  mime_types_[".pr"] = "application/x-pr";
  mime_types_[".prf"] = "application/pics-rules";
  mime_types_[".prn"] = "application/x-prn";
  mime_types_[".prt"] = "application/x-prt";
  mime_types_[".ps"] = "application/postscript";
  mime_types_[".ptn"] = "application/x-ptn";
  mime_types_[".pwz"] = "application/vnd.ms-powerpoint";
  mime_types_[".r3t"] = "text/vnd.rn-realtext3d";
  mime_types_[".ra"] = "audio/vnd.rn-realaudio";
  mime_types_[".ram"] = "audio/x-pn-realaudio";
  mime_types_[".rar"] = "application/x-rar-compressed";
  mime_types_[".ras"] = "application/x-ras";
  mime_types_[".rat"] = "application/rat-file";
  mime_types_[".rdf"] = "text/xml";
  mime_types_[".rec"] = "application/vnd.rn-recording";
  mime_types_[".red"] = "application/x-red";
  mime_types_[".rgb"] = "application/x-rgb";
  mime_types_[".rjs"] = "application/vnd.rn-realsystem-rjs";
  mime_types_[".rjt"] = "application/vnd.rn-realsystem-rjt";
  mime_types_[".rlc"] = "application/x-rlc";
  mime_types_[".rle"] = "application/x-rle";
  mime_types_[".rm"] = "audio/x-pn-realaudio";
  mime_types_[".rmf"] = "application/vnd.adobe.rmf";
  mime_types_[".rmi"] = "audio/mid";
  mime_types_[".rmj"] = "application/vnd.rn-realsystem-rmj";
  mime_types_[".rmm"] = "audio/x-pn-realaudio";
  mime_types_[".rmp"] = "application/vnd.rn-rn_music_package";
  mime_types_[".rms"] = "application/vnd.rn-realmedia-secure";
  mime_types_[".rmvb"] = "audio/x-pn-realaudio";
  mime_types_[".rmx"] = "application/vnd.rn-realsystem-rmx";
  mime_types_[".rnx"] = "application/vnd.rn-realplayer";
  mime_types_[".rp"] = "image/vnd.rn-realpix";
  mime_types_[".rpm"] = "audio/x-pn-realaudio-plugin";
  mime_types_[".rsml"] = "application/vnd.rn-rsml";
  mime_types_[".rt"] = "text/vnd.rn-realtext";
  mime_types_[".rtf"] = "application/rtf";
  mime_types_[".rv"] = "video/vnd.rn-realvideo";
  mime_types_[".sam"] = "application/x-sam";
  mime_types_[".sat"] = "application/x-sat";
  mime_types_[".sdp"] = "application/sdp";
  mime_types_[".sdw"] = "application/x-sdw";
  mime_types_[".shtml"] = "text/html";
  mime_types_[".sit"] = "application/x-stuffit";
  mime_types_[".slb"] = "application/x-slb";
  mime_types_[".sld"] = "application/x-sld";
  mime_types_[".slk"] = "drawing/x-slk";
  mime_types_[".smi"] = "application/smil";
  mime_types_[".smil"] = "application/smil";
  mime_types_[".smk"] = "application/x-smk";
  mime_types_[".snd"] = "audio/basic";
  mime_types_[".sol"] = "text/plain";
  mime_types_[".sor"] = "text/plain";
  mime_types_[".spc"] = "application/x-pkcs7-certificates";
  mime_types_[".spl"] = "application/futuresplash";
  mime_types_[".spp"] = "text/xml";
  mime_types_[".ssm"] = "application/streamingmedia";
  mime_types_[".sst"] = "application/vnd.ms-pki.certstore";
  mime_types_[".stl"] = "application/vnd.ms-pki.stl";
  mime_types_[".stm"] = "text/html";
  mime_types_[".sty"] = "application/x-sty";
  mime_types_[".svg"] = "text/xml";
  mime_types_[".swf"] = "application/x-shockwave-flash";
  mime_types_[".swfl"] = "application/x-shockwave-flash";
  mime_types_[".tar"] = "application/x-tar";
  mime_types_[".tdf"] = "application/x-tdf";
  mime_types_[".tg4"] = "application/x-tg4";
  mime_types_[".tga"] = "application/x-tga";
  mime_types_[".tgz"] = "application/x-tar";
  mime_types_[".tif"] = "application/x-tif";
  mime_types_[".tiff"] = "image/tiff";
  mime_types_[".tld"] = "text/xml";
  mime_types_[".top"] = "drawing/x-top";
  mime_types_[".torrent"] = "application/x-bittorrent";
  mime_types_[".tsd"] = "text/xml";
  mime_types_[".txt"] = "text/plain";
  mime_types_[".uin"] = "application/x-icq";
  mime_types_[".uls"] = "text/iuls";
  mime_types_[".vcf"] = "text/x-vcard";
  mime_types_[".vda"] = "application/x-vda";
  mime_types_[".vdx"] = "application/vnd.visio";
  mime_types_[".vml"] = "text/xml";
  mime_types_[".vpg"] = "application/x-vpeg005";
  mime_types_[".vsd"] = "application/x-vsd";
  mime_types_[".vss"] = "application/vnd.visio";
  mime_types_[".vst"] = "application/x-vst";
  mime_types_[".vsw"] = "application/vnd.visio";
  mime_types_[".vsx"] = "application/vnd.visio";
  mime_types_[".vtx"] = "application/vnd.visio";
  mime_types_[".vxml"] = "text/xml";
  mime_types_[".wav"] = "audio/wav";
  mime_types_[".wax"] = "audio/x-ms-wax";
  mime_types_[".wb1"] = "application/x-wb1";
  mime_types_[".wb2"] = "application/x-wb2";
  mime_types_[".wb3"] = "application/x-wb3";
  mime_types_[".wbmp"] = "image/vnd.wap.wbmp";
  mime_types_[".wiz"] = "application/msword";
  mime_types_[".wk3"] = "application/x-wk3";
  mime_types_[".wk4"] = "application/x-wk4";
  mime_types_[".wkq"] = "application/x-wkq";
  mime_types_[".wks"] = "application/x-wks";
  mime_types_[".wm"] = "video/x-ms-wm";
  mime_types_[".wma"] = "audio/x-ms-wma";
  mime_types_[".wmd"] = "application/x-ms-wmd";
  mime_types_[".wmf"] = "application/x-wmf";
  mime_types_[".wml"] = "text/vnd.wap.wml";
  mime_types_[".wmv"] = "audio/x-ms-wmv";
  mime_types_[".wmx"] = "video/x-ms-wmx";
  mime_types_[".wmz"] = "application/x-ms-wmz";
  mime_types_[".wp6"] = "application/x-wp6";
  mime_types_[".wpd"] = "application/x-wpd";
  mime_types_[".wpg"] = "application/x-wpg";
  mime_types_[".wpl"] = "application/vnd.ms-wpl";
  mime_types_[".wq1"] = "application/x-wq1";
  mime_types_[".wr1"] = "application/x-wr1";
  mime_types_[".wri"] = "application/x-wri";
  mime_types_[".wrk"] = "application/x-wrk";
  mime_types_[".ws"] = "application/x-ws";
  mime_types_[".ws2"] = "application/x-ws";
  mime_types_[".wsc"] = "text/scriptlet";
  mime_types_[".wsdl"] = "text/xml";
  mime_types_[".wvx"] = "video/x-ms-wvx";
  mime_types_[".x_b"] = "application/x-x_b";
  mime_types_[".x_t"] = "application/x-x_t";
  mime_types_[".xdp"] = "application/vnd.adobe.xdp";
  mime_types_[".xdr"] = "text/xml";
  mime_types_[".xfd"] = "application/vnd.adobe.xfd";
  mime_types_[".xfdf"] = "application/vnd.adobe.xfdf";
  mime_types_[".xht"] = "application/xhtml+xml";
  mime_types_[".xhtm"] = "application/xhtml+xml";
  mime_types_[".xhtml"] = "application/xhtml+xml";
  mime_types_[".xla"] = "application/msexcel";
  mime_types_[".xls"] = "application/msexcel";
  mime_types_[".xlw"] = "application/x-xlw";
  mime_types_[".xml"] = "text/xml";
  mime_types_[".xpl"] = "audio/scpls";
  mime_types_[".xq"] = "text/xml";
  mime_types_[".xql"] = "text/xml";
  mime_types_[".xquery"] = "text/xml";
  mime_types_[".xsd"] = "text/xml";
  mime_types_[".xsl"] = "text/xml";
  mime_types_[".xslt"] = "text/xml";
  mime_types_[".xwd"] = "application/x-xwd";
  mime_types_[".zip"] = "application/zip";
}

MimeType::~MimeType()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

const std::string& MimeType::operator()
  (const std::string& file_suffix) const
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  MimeTypesMap::const_iterator iter = mime_types_.find(file_suffix);

  if (mime_types_.end() != iter) {
    return (iter->second);
  } else {
    return (default_mime_type_);
  }
};

} // namespace flexnet
